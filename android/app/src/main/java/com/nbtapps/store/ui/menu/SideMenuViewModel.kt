// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.menu

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.ssh.SshAccess
import com.nbtapps.store.data.ssh.SshEntry
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.ui.restart.RestartQuestion
import com.nbtapps.store.ui.restart.RestartQuestions
import com.nbtapps.store.ui.ssh.SSH_CANT_RUN
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn

/** Where a tap on the menu's SSH entry goes. */
sealed interface SshMenuTarget {
    /** The Head units tab, to connect. */
    data object HeadUnits : SshMenuTarget

    /** The page of the package [packageId] in the catalogue, where it is installed from. */
    data class Package(val packageId: String) : SshMenuTarget

    /** The page of the head unit [headUnitId], where its agent is. */
    data class HeadUnit(val headUnitId: String) : SshMenuTarget

    /** The SSH page of the head unit [headUnitId]. */
    data class Ssh(val headUnitId: String) : SshMenuTarget
}

/**
 * The menu's section for the head unit the app is connected to, and its SSH entry. [label] is over it: the head unit's
 * name, or that none is connected. [line] is under SSH, if there is one; [greyed]: in greyed colours, though it may
 * still be tapped; [install]: "Install…" stands at its end; [dot]: SSH access is on, and a dot in the primary colour
 * says so beside the line. A tap goes to [target].
 */
data class SshMenuItem(
    val label: String,
    val line: String?,
    val greyed: Boolean,
    val install: Boolean,
    val dot: Boolean,
    val target: SshMenuTarget,
)

internal const val NO_HEAD_UNIT = "No head unit connected"
internal const val CONNECT_FIRST = "Connect to a head unit first"
internal const val NOT_INSTALLED = "SSH access isn't installed on this head unit"

/** The menu's SSH entry for [entry]: see [SshMenuItem]. */
fun sshMenuItem(entry: SshEntry): SshMenuItem = when (entry) {
    SshEntry.NotConnected -> SshMenuItem(NO_HEAD_UNIT, CONNECT_FIRST, greyed = true, install = false, dot = false, SshMenuTarget.HeadUnits)
    is SshEntry.NotInstalled ->
        SshMenuItem(entry.name, NOT_INSTALLED, greyed = true, install = true, dot = false, SshMenuTarget.Package(SshTunnel.PACKAGE))
    is SshEntry.CantRun -> SshMenuItem(entry.name, SSH_CANT_RUN, greyed = true, install = false, dot = false, SshMenuTarget.HeadUnit(entry.headUnitId))
    is SshEntry.Ready -> SshMenuItem(entry.name, entry.line, greyed = false, install = false, dot = entry.on == true, SshMenuTarget.Ssh(entry.headUnitId))
}

/**
 * The menu's entry Restart iDrive, under SSH. With a link up, to [headUnitId], it asks first whether to restart iDrive
 * there, whether SSH is there or not; with none it is [greyed], though it may still be tapped, [line] says to connect
 * first, and a tap opens the Head units tab, as the SSH entry's does then.
 */
data class RestartMenuItem(val headUnitId: String?) {
    val greyed: Boolean get() = headUnitId == null
    val line: String? get() = CONNECT_FIRST.takeIf { greyed }
}

/** The menu's entry Restart iDrive for [connection]: see [RestartMenuItem]. A link being made or waiting for pairing is none. */
fun restartMenuItem(connection: ConnectionState): RestartMenuItem = RestartMenuItem((connection as? ConnectionState.Connected)?.headUnitId)

/**
 * The side menu's view model: its section for the connected head unit, from the app's SSH switch ([SshAccess.entry])
 * and the app's link, and the question before iDrive restarts that its entry Restart iDrive asks, which stays once
 * the menu has closed.
 */
@HiltViewModel
class SideMenuViewModel internal constructor(
    entry: StateFlow<SshEntry>,
    /** The app's link: [JobRepository.connection]. */
    connection: StateFlow<ConnectionState>,
    restarts: RestartQuestions,
) : ViewModel() {

    @Inject
    constructor(access: SshAccess, jobs: JobRepository, restarts: RestartQuestions) : this(access.entry, jobs.connection, restarts)

    val ssh: StateFlow<SshMenuItem> =
        entry.map(::sshMenuItem).stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), sshMenuItem(entry.value))

    /** The entry Restart iDrive. */
    val restartItem: StateFlow<RestartMenuItem> =
        connection.map(::restartMenuItem).stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), restartMenuItem(connection.value))

    /** The question its tap asks. */
    val restart: RestartQuestion = restarts.question(viewModelScope)

    /**
     * A tap on Restart iDrive, as the menu showed it ([item]): asks the question about its head unit and says so, or,
     * greyed, says that the Head units tab is to be opened in its place.
     */
    fun tapRestart(item: RestartMenuItem): Boolean {
        val id = item.headUnitId ?: return false
        restart.ask(id)
        return true
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.ssh

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.ssh.SshAccess
import com.nbtapps.store.data.ssh.SshEntry
import com.nbtapps.store.data.ssh.SshPort
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.ssh.SshSwitchState
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.ui.headunits.ConnectGuard
import com.nbtapps.store.ui.navigation.SshRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** What the SSH page shows under its top bar and banner. */
sealed interface SshPageContent {
    /** The saved head units have not been read yet. */
    data object Loading : SshPageContent

    /** The head unit is not saved, as after it was removed. */
    data object NotFound : SshPageContent

    /** The SSH package is not on the head unit: the row that offers its install. */
    data object Install : SshPageContent

    /** The head unit's agent cannot run the SSH package: see [SshEntry.CantRun]. */
    data object CantRun : SshPageContent

    /** The switch, the terminal and the port. */
    data class Controls(val card: SshCard) : SshPageContent
}

data class SshUiState(
    /** The head unit's name, under the title. Null until it has been read. */
    val name: String? = null,
    /** The app's link is not to this head unit: the banner says so, and offers Connect. */
    val apart: Boolean = false,
    /** A connect to this head unit is under way: Connect waits for it. */
    val connecting: Boolean = false,
    val content: SshPageContent = SshPageContent.Loading,
    /** The question before SSH access is switched on is open. */
    val confirming: Boolean = false,
)

/** The banner's line while the app's link is not to the page's head unit. */
internal const val SSH_APART = "Not connected to the head unit. The switch comes back when it is."

/** The line of the page where the head unit's agent cannot run SSH access. */
internal const val SSH_CANT_RUN = "This head unit's agent can't run SSH access. Update the agent."

/**
 * The SSH page of the head unit [headUnitId], whose saved record is [unit] ([loaded] once read). With the app connected
 * to it, the menu's [entry] says what the page is: the install's row, the agent's refusal, or the card, whose switch is
 * [switch]. With the link down or to another head unit, the page stays: the banner says so, and the card is there where
 * the package was installed when the agent last said, with its switch off and greyed, as its state is not known; the
 * terminal's row and the port's follow [terminal] and [port] as they are for this head unit, which end with the link.
 */
internal fun sshPage(
    headUnitId: String,
    loaded: Boolean,
    unit: HeadUnit?,
    connection: ConnectionState,
    entry: SshEntry,
    switch: SshSwitchState,
    terminal: TerminalState,
    port: SshPortState,
): SshUiState {
    if (!loaded) return SshUiState()
    if (unit == null) return SshUiState(content = SshPageContent.NotFound)
    val connecting = connection is ConnectionState.Connecting && connection.headUnitId == headUnitId
    val mine = switch.takeIf { it.headUnitId == headUnitId }
    val content = when (entry) {
        is SshEntry.NotInstalled -> SshPageContent.Install.takeIf { entry.headUnitId == headUnitId }
        is SshEntry.CantRun -> SshPageContent.CantRun.takeIf { entry.headUnitId == headUnitId }
        is SshEntry.Ready -> if (entry.headUnitId != headUnitId) {
            null
        } else {
            SshPageContent.Controls(
                SshCard(
                    on = entry.on,
                    connections = mine?.connections,
                    switching = mine?.switching == true,
                    error = mine?.error,
                    terminalOpen = entry.terminalOpen,
                    port = entry.port,
                ),
            )
        }
        SshEntry.NotConnected -> null
    }
    if (content != null) {
        return SshUiState(unit.name, apart = false, connecting = false, content = content, confirming = mine?.confirming == true)
    }
    val installed = unit.profile?.installed.orEmpty().any { it.id == SshTunnel.PACKAGE }
    val apart = if (installed) {
        SshPageContent.Controls(
            SshCard(
                terminalOpen = terminal.active && terminal.headUnitId == headUnitId,
                port = port.takeIf { it.headUnitId == headUnitId } ?: SshPortState.Off(),
            ),
        )
    } else {
        SshPageContent.Install
    }
    return SshUiState(unit.name, apart = true, connecting = connecting, content = apart)
}

/**
 * The SSH page of one head unit ([SshRoute]): the SSH package's switch, the way to the terminal and to SSH from other
 * apps. The switch is the app's ([SshAccess]), which reads it again as the page opens.
 */
@HiltViewModel
class SshViewModel internal constructor(
    val headUnitId: String,
    unit: Flow<HeadUnit?>,
    connection: StateFlow<ConnectionState>,
    private val access: SshAccess,
    terminal: Flow<TerminalState>,
    port: Flow<SshPortState>,
    /** What the user's connect goes through first: [com.nbtapps.store.ui.headunits.ConnectGate]. */
    private val guard: ConnectGuard = ConnectGuard.NONE,
    /** The user's connect to a head unit: [JobRepository.connect]. */
    private val connectTo: suspend (headUnitId: String) -> Unit,
) : ViewModel() {

    @Inject
    constructor(
        savedStateHandle: SavedStateHandle,
        headUnits: HeadUnitRepository,
        jobs: JobRepository,
        access: SshAccess,
        terminal: TerminalRepository,
        port: SshPort,
        guard: ConnectGuard,
    ) : this(
        savedStateHandle.toRoute<SshRoute>().headUnitId,
        headUnits.headUnit(savedStateHandle.toRoute<SshRoute>().headUnitId),
        jobs.connection,
        access,
        terminal.state,
        port.state,
        guard,
        { id -> jobs.connect(id, byUser = true) },
    )

    private data class Side(val switch: SshSwitchState, val terminal: TerminalState, val port: SshPortState)

    val uiState: StateFlow<SshUiState> =
        combine(unit, connection, access.entry, combine(access.state, terminal, port, ::Side)) { unit, connection, entry, side ->
            sshPage(headUnitId, loaded = true, unit, connection, entry, side.switch, side.terminal, side.port)
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), SshUiState())

    init {
        access.refresh(headUnitId)
    }

    /** The switch thrown to [on]: on asks first ([confirm]), off is done at once. See [SshAccess.switch]. */
    fun switch(on: Boolean) = access.switch(headUnitId, on)

    /** The question before SSH access is switched on is answered yes. */
    fun confirm() = access.confirm(headUnitId)

    /** The question is answered no, or put away. */
    fun cancel() = access.cancel()

    /** Connect, from the banner: after the question where a terminal or the SSH port of another head unit would end. */
    fun connect() {
        viewModelScope.launch { if (guard.mayConnect(headUnitId)) connectTo(headUnitId) }
    }
}

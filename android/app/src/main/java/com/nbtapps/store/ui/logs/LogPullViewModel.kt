// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.logs

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.logs.LogPull
import com.nbtapps.store.data.logs.LogPullRequest
import com.nbtapps.store.data.logs.LogPullState
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.model.AgentFile
import com.nbtapps.store.ui.navigation.LogPullRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import java.io.File
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** What the head unit said when asked for the package's log files. */
sealed interface Listing {
    data object Loading : Listing

    data class Ready(val files: List<AgentFile>, val more: Boolean) : Listing

    data class Failed(val reason: String) : Listing
}

data class LogPullUiState(
    val packageId: String,
    /** The package's name as the head unit lists it, or its id. */
    val name: String,
    /** There is a link to this head unit, and its agent can send files. */
    val available: Boolean = false,
    val listing: Listing = Listing.Loading,
    /** The paths of the listed files that are to be pulled: all of them, until the user takes some out. */
    val chosen: Set<String> = emptySet(),
    /** Only the last [LogPullViewModel.TAIL_BYTES] of each file. */
    val tail: Boolean = false,
    /** This package's pull that runs, or its last one while this screen has been open. */
    val pull: LogPullState? = null,
    /** A ZIP of this screen's own pull that has just been made, to hand to the share sheet once. */
    val shareNow: File? = null,
) {
    val files: List<AgentFile> get() = (listing as? Listing.Ready)?.files.orEmpty()

    val chosenFiles: List<AgentFile> get() = files.filter { it.path in chosen }

    /** Bytes the chosen files come to before compression, with the tail if it is asked for. */
    val bytes: Long get() = chosenFiles.sumOf { LogPull.wanted(it, if (tail) LogPullViewModel.TAIL_BYTES else null) }

    /** At most how long the chosen files take over Bluetooth serial, in seconds. Compressed, they mostly take far less. */
    val estimateSeconds: Long get() = (bytes + LogPullViewModel.SERIAL_BYTES_PER_SECOND - 1) / LogPullViewModel.SERIAL_BYTES_PER_SECOND

    val pulling: Boolean get() = pull is LogPullState.Pulling

    val canPull: Boolean get() = available && !pulling && chosen.isNotEmpty()

    /** The ZIP of the last pull, while it is still there to share again. */
    val zip: File? get() = (pull as? LogPullState.Done)?.zip?.takeIf { it.isFile }
}

/**
 * Pulls log files of one package, or of the agent itself, from the connected head unit into a ZIP and hands it
 * to Android's share sheet: the files are listed, all of them chosen, and the user takes out what they don't
 * want or asks for the end of each only. The pull belongs to [JobRepository], so it goes on when the screen is left.
 */
@HiltViewModel
class LogPullViewModel internal constructor(
    private val headUnitId: String,
    private val packageId: String,
    private val jobs: JobRepository,
    private val appVersion: String,
) : ViewModel() {

    @Inject
    constructor(savedStateHandle: SavedStateHandle, jobs: JobRepository, app: InstalledApp) : this(
        savedStateHandle.toRoute<LogPullRoute>().headUnitId,
        savedStateHandle.toRoute<LogPullRoute>().packageId,
        jobs,
        app.versionName,
    )

    private data class Local(
        val listing: Listing = Listing.Loading,
        val chosen: Set<String> = emptySet(),
        val tail: Boolean = false,
        /** A pull was started from this screen and its ZIP has not been handed to the share sheet yet. */
        val started: Boolean = false,
        val shared: File? = null,
    )

    private val local = MutableStateFlow(Local())

    val uiState: StateFlow<LogPullUiState> = combine(jobs.connection, jobs.logPull, local) { connection, pull, local ->
        val connected = (connection as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }
        val mine = pull?.takeIf { it.packageId == packageId }
        val done = (mine as? LogPullState.Done)?.zip
        LogPullUiState(
            packageId = packageId,
            name = connected?.installed?.firstOrNull { it.id == packageId }?.name ?: if (packageId == AGENT_PACKAGE_ID) "the agent" else packageId,
            available = connected?.files == true,
            listing = local.listing,
            chosen = local.chosen,
            tail = local.tail,
            pull = mine,
            shareNow = done?.takeIf { local.started && it != local.shared },
        )
    }.stateIn(viewModelScope, SharingStarted.Eagerly, LogPullUiState(packageId, packageId))

    init {
        reload()
    }

    /** Asks the head unit for the package's log files again. All of them are chosen. */
    fun reload() {
        viewModelScope.launch {
            local.update { it.copy(listing = Listing.Loading) }
            val result = jobs.listLogs(packageId)
            local.update { state ->
                result.fold(
                    onSuccess = { listing -> state.copy(listing = Listing.Ready(listing.files, listing.more), chosen = listing.files.map { it.path }.toSet()) },
                    onFailure = { failure -> state.copy(listing = Listing.Failed(failure.message ?: "The head unit didn't answer")) },
                )
            }
        }
    }

    fun choose(path: String, chosen: Boolean) = local.update { it.copy(chosen = if (chosen) it.chosen + path else it.chosen - path) }

    fun setTail(on: Boolean) = local.update { it.copy(tail = on) }

    fun start() {
        val state = uiState.value
        if (!state.canPull) return
        val listing = state.listing as? Listing.Ready ?: return
        local.update { it.copy(started = true) }
        jobs.startPull(LogPullRequest(packageId, state.chosenFiles, if (state.tail) TAIL_BYTES else null, listingCut = listing.more), appVersion)
    }

    fun cancel() = jobs.cancelPull()

    /** The share sheet has been shown for [zip]: it is not shown again by itself, only when asked. */
    fun shared(zip: File) = local.update { it.copy(shared = zip) }

    companion object {
        /** The end of each file, where only that is asked for: what a log's latest goings-on mostly fit in. */
        const val TAIL_BYTES = 256L * 1024

        /** What Bluetooth serial carries, about: for the estimate of how long a pull takes. */
        const val SERIAL_BYTES_PER_SECOND = 20L * 1024
    }
}

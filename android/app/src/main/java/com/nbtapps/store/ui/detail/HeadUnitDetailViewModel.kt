// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.serialUnavailable
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.ui.navigation.HeadUnitDetailRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** The link to the agent on this head unit. */
sealed interface LinkState {
    /** No endpoint of this head unit can be reached by this build of the app. */
    data object Unsupported : LinkState
    data object Idle : LinkState
    data object Connecting : LinkState
    data class Connected(val over: String) : LinkState

    /** A link is open, and the agent wants this phone paired before it says more. [stale]: it knew the phone once. */
    data class NeedsPairing(val stale: Boolean) : LinkState
    data class Failed(val reason: String) : LinkState
}

/**
 * Whether a Bluetooth serial link to the head unit could be tried right now, as the phone's own Bluetooth tells
 * it: nothing is opened to find out, so it says nothing of whether the car is in range or the agent runs.
 */
data class SerialStatus(
    /** Why a link cannot be tried, in words for the user. Null if it can: Bluetooth is on and the car is paired. */
    val problem: String?,
    /** The problem is the missing Nearby devices permission, which the user can grant from here. */
    val needsPermission: Boolean,
)

/** What the phone's Bluetooth says of [unit]'s Bluetooth serial endpoint, the first if it has several. Null if it has none. */
internal fun PhoneBluetooth.serialStatus(unit: HeadUnit?): SerialStatus? =
    unit?.endpoints?.filterIsInstance<SerialEndpoint>()?.firstOrNull()?.let {
        SerialStatus(serialUnavailable(it.address), needsPermission = access() == BluetoothAccess.NOT_ALLOWED)
    }

data class HeadUnitDetailUiState(
    /** False until the saved head units have been read. */
    val loaded: Boolean = false,
    val headUnit: HeadUnit? = null,
    /** For a head unit with a Bluetooth serial endpoint. Null without one, and until it has been read. */
    val serial: SerialStatus? = null,
    val link: LinkState = LinkState.Idle,
    /** What the agent reports now, or reported last time when there is no link. */
    val profile: HeadUnitProfile? = null,
    val installed: List<InstalledPackage> = emptyList(),
    /** The running or last job on this head unit. */
    val job: JobView? = null,
    /** Why the last restart request failed. */
    val restartError: String? = null,
    /**
     * Whether the agent is started with the car, as it says over the link that is open. Null with no link, and
     * for an agent that has no such switch.
     */
    val autostart: Autostart? = null,
    /** The switch has been thrown, and the agent has not answered yet. */
    val autostartBusy: Boolean = false,
    /** Why the agent did not take the last throw of the switch. */
    val autostartError: String? = null,
    /** There is a link to this head unit, and its agent can send log files: see [com.nbtapps.store.ui.logs.LogPullScreen]. */
    val logs: Boolean = false,
)

@HiltViewModel
class HeadUnitDetailViewModel @Inject constructor(
    savedStateHandle: SavedStateHandle,
    private val repository: HeadUnitRepository,
    private val jobs: JobRepository,
    private val connector: LinkConnector,
    private val bluetooth: PhoneBluetooth,
) : ViewModel() {

    private val headUnitId = savedStateHandle.toRoute<HeadUnitDetailRoute>().headUnitId

    private data class Local(
        val serial: SerialStatus? = null,
        val restartError: String? = null,
        val autostartBusy: Boolean = false,
        val autostartError: String? = null,
    )

    private val local = MutableStateFlow(Local())

    val uiState: StateFlow<HeadUnitDetailUiState> =
        combine(repository.headUnit(headUnitId), jobs.connection, jobs.job, local) { unit, connection, job, local ->
            val connected = (connection as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }
            HeadUnitDetailUiState(
                loaded = true,
                headUnit = unit,
                serial = local.serial,
                link = when {
                    unit == null || unit.endpoints.none(connector::supports) -> LinkState.Unsupported
                    connected != null -> LinkState.Connected(connected.endpoint.label)
                    connection is ConnectionState.Connecting && connection.headUnitId == headUnitId -> LinkState.Connecting
                    connection is ConnectionState.NeedsPairing && connection.headUnitId == headUnitId -> LinkState.NeedsPairing(connection.stale)
                    connection is ConnectionState.Failed && connection.headUnitId == headUnitId -> LinkState.Failed(connection.reason)
                    else -> LinkState.Idle
                },
                profile = unit?.profile,
                installed = connected?.installed ?: unit?.profile?.installed.orEmpty(),
                job = job?.takeIf { it.headUnitId == headUnitId },
                restartError = local.restartError,
                autostart = connected?.autostart,
                autostartBusy = local.autostartBusy,
                autostartError = local.autostartError,
                logs = connected?.files == true,
            )
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), HeadUnitDetailUiState())

    /**
     * Reads what the phone's Bluetooth says of the head unit's serial endpoint: when the screen comes up, on
     * return to it, and after the permission prompt. Pairing and switching Bluetooth on happen outside the app.
     */
    fun refreshBluetooth() {
        viewModelScope.launch {
            val status = bluetooth.serialStatus(repository.headUnit(headUnitId).first())
            local.update { it.copy(serial = status) }
        }
    }

    fun connect() {
        viewModelScope.launch { jobs.connect(headUnitId) }
    }

    fun disconnect() {
        viewModelScope.launch { jobs.disconnect() }
    }

    /**
     * Has the agent started with the car from now on, or no longer. Switching it on where it is on already is
     * not idle: the agent then forgets the starts on record that did not last, and the car tries again.
     */
    fun setAutostart(on: Boolean) {
        if (local.value.autostartBusy) return
        viewModelScope.launch {
            local.update { it.copy(autostartBusy = true, autostartError = null) }
            val failure = jobs.setAutostart(on).exceptionOrNull()
            local.update { it.copy(autostartBusy = false, autostartError = failure?.let { why -> why.message ?: "The head unit didn't answer" }) }
        }
    }

    /** [kind] is `hmi` or `reboot`. */
    fun restart(kind: String) {
        viewModelScope.launch {
            local.update { it.copy(restartError = null) }
            jobs.restart(kind).onFailure { failure -> local.update { it.copy(restartError = failure.message) } }
            jobs.refresh()
        }
    }
}

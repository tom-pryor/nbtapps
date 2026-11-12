// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.jobs.headUnitId
import com.nbtapps.store.data.jobs.selectedHeadUnit
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.di.ApplicationScope
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.HeadUnit
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** What the link to the selected head unit is doing. [label] is read out with the head unit's name. */
enum class IndicatorStatus(val label: String) {
    CONNECTED("Connected"),
    CONNECTING("Connecting"),

    /** A link is open, and the head unit wants this phone paired before it says more over it. */
    NEEDS_PAIRING("Not paired"),
    FAILED("Connection failed"),
    IDLE("Not connected"),
}

data class IndicatorHeadUnit(val id: String, val name: String)

/** The selected head unit and the app's link to it, for the indicator in a top bar and the menu under it. */
data class HeadUnitIndicatorUiState(
    /** False until the saved head units have been read. */
    val loaded: Boolean = false,
    /** The selected head unit. Null when none is saved. */
    val headUnit: IndicatorHeadUnit? = null,
    val status: IndicatorStatus = IndicatorStatus.IDLE,
    /** Under the name in the menu: what the link runs over, or why there is none. */
    val detail: String = "",
    /** Whether this build has a way to the selected head unit at all, so whether a connect is offered. */
    val canConnect: Boolean = false,
    /** The other saved head units this build can reach, to connect to in its place. */
    val others: List<IndicatorHeadUnit> = emptyList(),
    /** A job is running over the link, which a connect to another head unit would take from under it. */
    val busy: Boolean = false,
)

/** What the indicator shows for the saved [units], the app's [connection] and its [job]. [supports] is the build's. */
internal fun indicatorState(
    units: List<HeadUnit>,
    connection: ConnectionState,
    job: JobView?,
    supports: (Endpoint) -> Boolean,
): HeadUnitIndicatorUiState {
    val selected = selectedHeadUnit(units, connection) ?: return HeadUnitIndicatorUiState(loaded = true)
    val reachable = selected.endpoints.any(supports)
    // A state that names another head unit is none of this one's: that one was removed since.
    val own = connection.takeIf { it.headUnitId == selected.id }
    return HeadUnitIndicatorUiState(
        loaded = true,
        headUnit = IndicatorHeadUnit(selected.id, selected.name),
        status = when (own) {
            is ConnectionState.Connected -> IndicatorStatus.CONNECTED
            is ConnectionState.Connecting -> IndicatorStatus.CONNECTING
            is ConnectionState.NeedsPairing -> IndicatorStatus.NEEDS_PAIRING
            is ConnectionState.Failed -> IndicatorStatus.FAILED
            else -> IndicatorStatus.IDLE
        },
        detail = when {
            own is ConnectionState.Connected -> "Connected: ${own.endpoint.label}"
            own is ConnectionState.Connecting -> "Connecting…"
            own is ConnectionState.NeedsPairing ->
                if (own.stale) "The head unit no longer knows this phone. Pair it again." else "This phone isn't paired with the head unit yet"
            own is ConnectionState.Failed -> own.reason
            !reachable -> "This version of the app can't open a link to this head unit"
            else -> "Not connected"
        },
        canConnect = reachable,
        others = units.filter { it.id != selected.id && it.endpoints.any(supports) }.map { IndicatorHeadUnit(it.id, it.name) },
        busy = own is ConnectionState.Connected && job != null && job.headUnitId == selected.id && !job.ended && !job.linkLost,
    )
}

@HiltViewModel
class HeadUnitIndicatorViewModel @Inject constructor(
    headUnits: HeadUnitRepository,
    private val jobs: JobRepository,
    connector: LinkConnector,
    @ApplicationScope private val appScope: CoroutineScope,
) : ViewModel() {

    val uiState: StateFlow<HeadUnitIndicatorUiState> =
        combine(headUnits.headUnits, jobs.connection, jobs.job) { units, connection, job ->
            indicatorState(units, connection, job, connector::supports)
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), HeadUnitIndicatorUiState())

    /**
     * Opens the app's link to the head unit [id], in place of any it has. The attempt belongs to the app, not to
     * the screen it was asked for on: the indicator is on several screens, and a Bluetooth serial connect is tried
     * for twenty seconds, so the user may well have moved between them before it is settled.
     */
    fun connect(id: String) {
        appScope.launch { jobs.connect(id) }
    }

    fun disconnect() {
        appScope.launch { jobs.disconnect() }
    }
}

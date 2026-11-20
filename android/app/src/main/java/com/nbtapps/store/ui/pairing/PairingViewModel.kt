// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairing

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.link.SerialDiagnostics
import com.nbtapps.store.di.ApplicationScope
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.ui.headunits.CheckFixFlow
import com.nbtapps.store.ui.headunits.CheckFixStep
import com.nbtapps.store.ui.headunits.ConnectGuard
import com.nbtapps.store.ui.navigation.PairingRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** Where pairing this phone with one head unit stands, as the app's link to it tells. */
sealed interface PairingStep {
    /** A link is being opened. */
    data object Connecting : PairingStep

    /**
     * The link is up and the agent waits for the code. [stale]: it knew this phone once and no longer does.
     * [autostart]: the agent has the switch that has it start with the car, so the screen offers to set that up
     * once the phone is paired.
     */
    data class EnterCode(val stale: Boolean, val autostart: Boolean = false) : PairingStep

    /** The agent takes this phone: it is paired, now or from before. */
    data object Paired : PairingStep

    /** The app has no link to this head unit, and the code can go over nothing else. [reason]: why, if one just failed. */
    data class NoLink(val reason: String?) : PairingStep
}

data class PairingUiState(
    /** False until the saved head units have been read. */
    val loaded: Boolean = false,
    /** Null if the head unit is not saved any more. */
    val headUnitName: String? = null,
    val step: PairingStep = PairingStep.NoLink(null),
    /** What has been typed of the code: digits only, six at most. */
    val code: String = "",
    /** The code is with the agent, and its answer is awaited. */
    val working: Boolean = false,
    /** Why the last code did not pair the phone. */
    val error: String? = null,
    /** The box that has the head unit set up to start the agent with the car once the phone is paired: ticked to begin with. */
    val enableAutostart: Boolean = true,
    /** The set-up for this head unit, if there is one: see [AutostartSetup]. */
    val setup: AutostartStep? = null,
    /** The agent says over the link to this head unit that iDrive has to restart. */
    val restartNeeded: Boolean = false,
    /** How many problems the agent's checks found on this head unit that are not put aside, as is kept with it. */
    val openChecks: Int = 0,
    /** The confirmation of what the set-up asks to remove, while it is open. */
    val fix: CheckFixStep? = null,
) {
    val canPair: Boolean get() = step is PairingStep.EnterCode && code.length == ProtocolConstants.PAIRING_CODE_DIGITS && !working

    /**
     * The line that leads to the head unit's page for what its checks found: once the phone is paired, where no
     * set-up runs or is shown, which would ask about them itself.
     */
    val showsChecks: Boolean get() = step == PairingStep.Paired && setup == null && openChecks > 0
}

/** The step for the head unit [headUnitId] when the app's link stands as [connection] says. A link to another head unit is none to this one. */
internal fun pairingStep(headUnitId: String, connection: ConnectionState): PairingStep = when (connection) {
    is ConnectionState.NeedsPairing -> if (connection.headUnitId == headUnitId) {
        PairingStep.EnterCode(connection.stale, autostart = ProtocolConstants.CAPABILITY_AUTOSTART in connection.capabilities)
    } else {
        PairingStep.NoLink(null)
    }
    is ConnectionState.Connected -> if (connection.headUnitId == headUnitId) PairingStep.Paired else PairingStep.NoLink(null)
    is ConnectionState.Connecting -> if (connection.headUnitId == headUnitId) PairingStep.Connecting else PairingStep.NoLink(null)
    is ConnectionState.Failed -> PairingStep.NoLink(connection.reason.takeIf { connection.headUnitId == headUnitId })
    ConnectionState.Disconnected -> PairingStep.NoLink(null)
}

/**
 * Whether pairing from [step], with the box [ticked], goes on to set the head unit up to start the agent with the
 * car: only where the box was shown, which is where the agent has the switch.
 */
internal fun setsUpAutostart(ticked: Boolean, step: PairingStep): Boolean = ticked && (step as? PairingStep.EnterCode)?.autostart == true

/**
 * Pairs this phone with one head unit: the user opens the pairing screen on the car, which shows a code, and
 * types the code here. It goes to the agent over the link the app already has to it, which waits for just that
 * ([ConnectionState.NeedsPairing]).
 *
 * Where the agent can be started with the car, a box offers that, ticked to begin with: once the phone is paired the
 * head unit is set up for it ([AutostartSetup]). The tick is kept in the saved state only, for this pairing.
 */
@HiltViewModel
class PairingViewModel @Inject constructor(
    private val savedStateHandle: SavedStateHandle,
    headUnits: HeadUnitRepository,
    private val jobs: JobRepository,
    private val setup: AutostartSetup,
    @ApplicationScope private val appScope: CoroutineScope,
    private val diagnostics: SerialDiagnostics,
    /** What the user's connect goes through first: [com.nbtapps.store.ui.headunits.ConnectGate]. */
    private val guard: ConnectGuard,
) : ViewModel() {

    /** This app's id of the head unit being paired with. */
    val headUnitId = savedStateHandle.toRoute<PairingRoute>().headUnitId

    /** What "Copy diagnostics" copies, where the connect failed: see [SerialDiagnostics.report]. */
    fun diagnostics(): String = diagnostics.report(uiState.value.headUnitName, jobs.agentSerial(headUnitId))

    private val enableAutostart = savedStateHandle.getStateFlow(ENABLE_AUTOSTART, true)

    /** The code as typed, and handing it to the agent. */
    private val codes = CodePairing(jobs, setup, appScope)

    /** The confirmation of what the set-up asks to remove: the set-up runs the fix once the user has said yes. */
    private val fixes = CheckFixFlow(jobs, appScope)

    private val page =
        combine(headUnits.headUnit(headUnitId), jobs.connection, codes.state, enableAutostart, setup.state) { unit, connection, entry, enable, setup ->
            PairingUiState(
                loaded = true,
                headUnitName = unit?.name,
                step = pairingStep(headUnitId, connection),
                code = entry.code,
                working = entry.working,
                error = entry.error,
                enableAutostart = enable,
                setup = setup?.takeIf { it.headUnitId == headUnitId }?.step,
                restartNeeded = (connection as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }?.info?.restartNeeded == RestartKind.HMI,
                openChecks = unit?.checks?.open?.size ?: 0,
            )
        }

    val uiState: StateFlow<PairingUiState> =
        combine(page, fixes.state) { page, fix -> page.copy(fix = fix) }
            .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), PairingUiState())

    /** Ticks or clears the box that has the head unit set up to start the agent with the car. */
    fun setEnableAutostart(on: Boolean) {
        savedStateHandle[ENABLE_AUTOSTART] = on
    }

    fun setCode(typed: String) = codes.setCode(typed)

    /**
     * Hands the code to the agent. The attempt belongs to the app, not to this screen: once the agent has issued
     * the phone a key, the key has to be kept and proved whether or not the user is still looking. See [CodePairing].
     */
    fun pair() {
        // As the box stood when Pair was tapped.
        codes.pair(headUnitId, autostart = setsUpAutostart(enableAutostart.value, pairingStep(headUnitId, jobs.connection.value)))
    }

    /** Opens the app's link to the head unit, for the code to go over. As in the top bar's menu, the attempt is the app's. */
    fun connect() {
        appScope.launch { if (guard.mayConnect(headUnitId)) jobs.connect(headUnitId) }
    }

    /** Runs the set-up again from the top, after it stopped short. */
    fun retrySetup() = setup.retry()

    /** Asks the agent to restart iDrive, once the set-up is done: see [AutostartSetup.restart]. */
    fun restartIdrive() = setup.restart()

    /** The fix's button on the set-up's card: what it moves aside and removes, to be confirmed. */
    fun openRemoval() = fixes.openForSetup(setup, headUnitId)

    /** Skip on the set-up's card: see [AutostartSetup.skipRemoval]. */
    fun skipRemoval() = setup.skipRemoval()

    fun confirmRemoval() = fixes.confirm()

    fun retryRemoval() = fixes.retry()

    fun closeRemoval() = fixes.close()

    private companion object {
        /** Whether the box is ticked. */
        const val ENABLE_AUTOSTART = "enableAutostart"
    }
}

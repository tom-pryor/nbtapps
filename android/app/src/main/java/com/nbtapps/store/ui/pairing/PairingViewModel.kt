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
import com.nbtapps.store.di.ApplicationScope
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.ui.navigation.PairingRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
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
) {
    val canPair: Boolean get() = step is PairingStep.EnterCode && code.length == ProtocolConstants.PAIRING_CODE_DIGITS && !working
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

/** What of [typed] is a code so far: its digits, as the agent reads them, and no more of them than a code has. */
internal fun codeDigits(typed: String): String = typed.filter { it in '0'..'9' }.take(ProtocolConstants.PAIRING_CODE_DIGITS)

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
) : ViewModel() {

    private val headUnitId = savedStateHandle.toRoute<PairingRoute>().headUnitId

    private val enableAutostart = savedStateHandle.getStateFlow(ENABLE_AUTOSTART, true)

    private data class Local(val code: String = "", val working: Boolean = false, val error: String? = null)

    private val local = MutableStateFlow(Local())

    val uiState: StateFlow<PairingUiState> =
        combine(headUnits.headUnit(headUnitId), jobs.connection, local, enableAutostart, setup.state) { unit, connection, local, enable, setup ->
            PairingUiState(
                loaded = true,
                headUnitName = unit?.name,
                step = pairingStep(headUnitId, connection),
                code = local.code,
                working = local.working,
                error = local.error,
                enableAutostart = enable,
                setup = setup?.takeIf { it.headUnitId == headUnitId }?.step,
                restartNeeded = (connection as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }?.info?.restartNeeded == RestartKind.HMI,
            )
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), PairingUiState())

    /** Ticks or clears the box that has the head unit set up to start the agent with the car. */
    fun setEnableAutostart(on: Boolean) {
        savedStateHandle[ENABLE_AUTOSTART] = on
    }

    fun setCode(typed: String) {
        local.update { it.copy(code = codeDigits(typed), error = null) }
    }

    /**
     * Hands the code to the agent. The attempt belongs to the app, not to this screen: once the agent has issued
     * the phone a key, the key has to be kept and proved whether or not the user is still looking.
     */
    fun pair() {
        val code = local.value.code
        if (local.value.working || code.length != ProtocolConstants.PAIRING_CODE_DIGITS) return
        // As the box stood when Pair was tapped.
        val autostart = setsUpAutostart(enableAutostart.value, pairingStep(headUnitId, jobs.connection.value))
        local.update { it.copy(working = true, error = null) }
        appScope.launch {
            val result = jobs.pair(code)
            if (result.isSuccess && autostart) setup.begin(headUnitId)
            // A code that was wrong is not typed over: it is cleared for the next one.
            local.update { Local(error = result.exceptionOrNull()?.let { it.message ?: "The phone couldn't be paired" }) }
        }
    }

    /** Opens the app's link to the head unit, for the code to go over. As in the top bar's menu, the attempt is the app's. */
    fun connect() {
        appScope.launch { jobs.connect(headUnitId) }
    }

    /** Runs the set-up again from the top, after it stopped short. */
    fun retrySetup() = setup.retry()

    /** Asks the agent to restart iDrive, once the set-up is done: see [AutostartSetup.restart]. */
    fun restartIdrive() = setup.restart()

    private companion object {
        /** Whether the box is ticked. */
        const val ENABLE_AUTOSTART = "enableAutostart"
    }
}

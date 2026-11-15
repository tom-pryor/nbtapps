// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.PairingNeeded
import com.nbtapps.store.di.ApplicationScope
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.RestartKind
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * A clock in milliseconds that only goes forward and goes on while the phone sleeps, and across a restart of the
 * app's process: `SystemClock.elapsedRealtime` in the app. A countdown kept as a time on it survives both.
 */
fun interface ElapsedClock {
    fun millis(): Long
}

/** Where pairing this phone with the code another app handed over stands. */
sealed interface PairRequestStep {
    /** The code is shown, and the car is chosen or to be chosen. Nothing is connected until the user taps Pair. */
    data object Prompt : PairRequestStep

    /** A link to the chosen car is being opened. */
    data object Connecting : PairRequestStep

    /** The code is with the agent, and its answer is awaited. */
    data object Pairing : PairRequestStep

    /** The agent took the code: the phone is paired with [name]. */
    data class Paired(val name: String) : PairRequestStep

    /** The agent took the phone as it was: it was paired with [name] before. Nothing was sent. */
    data class AlreadyPaired(val name: String) : PairRequestStep

    /** No link to the car, so the code could not go to it. [reason] is for the user. */
    data class NoLink(val reason: String) : PairRequestStep

    /** The agent refused the code. [message] says why, in this screen's words. */
    data class Refused(val message: String) : PairRequestStep

    /** The code ran out before it could be sent. */
    data object TimeUp : PairRequestStep
}

data class PairRequestUiState(
    /** False until the saved head units have been read. */
    val loaded: Boolean = false,
    /** The code handed over. Null until a request has come. */
    val code: String? = null,
    val headUnits: List<HeadUnit> = emptyList(),
    /** The car the code goes to: the user's choice, or the one there is to choose. Null while none is chosen. */
    val chosen: HeadUnit? = null,
    /** How long the code is still good for, in milliseconds, or null if the other app did not say. */
    val remainingMs: Long? = null,
    val step: PairRequestStep = PairRequestStep.Prompt,
    /**
     * The box that has the car set up to start the agent with it once the phone is paired, ticked to begin with.
     * With it ticked the screen stays once the phone is paired, to show how that goes.
     */
    val enableAutostart: Boolean = true,
    /** The set-up for the chosen car, if there is one: see [AutostartSetup]. */
    val setup: AutostartStep? = null,
    /** The agent says over the link to the chosen car that iDrive has to restart. */
    val restartNeeded: Boolean = false,
) {
    val canPair: Boolean get() = step == PairRequestStep.Prompt && chosen != null && code != null

    /** A failed connect can be tried again while the code is still good. */
    val canRetry: Boolean get() = step is PairRequestStep.NoLink && chosen != null && (remainingMs == null || remainingMs > 0)
}

/**
 * The car the code goes to among [units]: [picked] while it is saved; else the only one; else, of several, the one
 * the app's link refers to as it stands in [connection]; else none, and the user picks.
 */
internal fun chosenHeadUnit(units: List<HeadUnit>, picked: String?, connection: ConnectionState): HeadUnit? {
    units.firstOrNull { it.id == picked }?.let { return it }
    units.singleOrNull()?.let { return it }
    val linked = when (connection) {
        is ConnectionState.Connected -> connection.headUnitId
        is ConnectionState.NeedsPairing -> connection.headUnitId
        is ConnectionState.Connecting -> connection.headUnitId
        is ConnectionState.Failed -> connection.headUnitId
        ConnectionState.Disconnected -> null
    }
    return units.firstOrNull { it.id == linked }
}

/**
 * Why the agent on [name] refused a handed-over code, by its [code]. Unlike [AgentText.pairing] it says nothing of
 * the car's screen: the code did not come from there, and cannot be changed here.
 */
internal fun pairRequestRefusal(code: String, name: String): String = when (code) {
    "forbidden" -> "$name isn't waiting for a pairing code just now. The code has run out or been used, or it is for another car."
    "rejected" -> "$name didn't take the code. It is for another car, or no longer good."
    "unsupported" -> "The agent on $name is too old to pair a phone."
    "bad-request" -> "$name didn't take the code."
    else -> AgentText.request(code)
}

/**
 * Pairs this phone with a head unit by a code that another app handed over: see docs/pairing-code-handover.md. The
 * user chooses the car, or adds it, and on one tap of Pair the app connects to it and sends the code over that
 * link, which the agent holds open waiting for it ([ConnectionState.NeedsPairing]). A phone the agent already
 * takes is paired already, and nothing is sent.
 *
 * A box, ticked to begin with, has the car set up to start the agent with it ([AutostartSetup]) once the phone is
 * paired, now or from before. Nothing is known of the agent before Pair, so the box is always shown.
 *
 * The code and its deadline are kept in the saved state, so a rotation or a restart of the process neither loses
 * the request nor starts its countdown again.
 */
@HiltViewModel
class PairRequestViewModel @Inject constructor(
    private val savedStateHandle: SavedStateHandle,
    headUnits: HeadUnitRepository,
    private val jobs: JobRepository,
    private val setup: AutostartSetup,
    @ApplicationScope private val appScope: CoroutineScope,
    private val clock: ElapsedClock,
) : ViewModel() {

    /** The saved head units, read from the start: Pair and an added car look at them at once. Null until read. */
    private val saved: StateFlow<List<HeadUnit>?> = headUnits.headUnits.stateIn(viewModelScope, SharingStarted.Eagerly, null)

    private val step = MutableStateFlow<PairRequestStep>(PairRequestStep.Prompt)

    /**
     * Rises with each new request. An attempt that was begun for an earlier one is left to end by itself, and what
     * it ends in is dropped.
     */
    @Volatile
    private var request = 0

    private val code = savedStateHandle.getStateFlow<String?>(CODE, null)
    private val deadline = savedStateHandle.getStateFlow<Long?>(DEADLINE, null)
    private val picked = savedStateHandle.getStateFlow<String?>(PICKED, null)
    private val enableAutostart = savedStateHandle.getStateFlow(ENABLE_AUTOSTART, true)

    /** The clock's time, read again each time the countdown's shown second changes. Null with no deadline. */
    @OptIn(ExperimentalCoroutinesApi::class)
    private val now: Flow<Long?> = deadline.flatMapLatest { until ->
        if (until == null) {
            flowOf(null)
        } else {
            flow<Long?> {
                while (true) {
                    val time = clock.millis()
                    emit(time)
                    if (time >= until) break
                    delay((until - time) % 1000L + 1)
                }
            }
        }
    }

    private val chosen = combine(saved, picked, jobs.connection) { units, picked, connection ->
        units?.let { it to chosenHeadUnit(it, picked, connection) }
    }

    private val pairing: Flow<PairRequestUiState> =
        combine(chosen, code, deadline, now, step) { units, code, deadline, now, step ->
            val remaining = if (deadline != null && now != null) (deadline - now).coerceAtLeast(0L) else null
            PairRequestUiState(
                loaded = units != null,
                code = code,
                headUnits = units?.first.orEmpty(),
                chosen = units?.second,
                remainingMs = remaining,
                // The code runs out while the user looks at it: there is nothing to tap Pair for then.
                step = if (step == PairRequestStep.Prompt && remaining == 0L) PairRequestStep.TimeUp else step,
            )
        }

    val uiState: StateFlow<PairRequestUiState> =
        combine(pairing, enableAutostart, setup.state, jobs.connection) { state, enable, setup, connection ->
            val id = state.chosen?.id
            state.copy(
                enableAutostart = enable,
                setup = setup?.takeIf { it.headUnitId == id }?.step,
                restartNeeded = (connection as? ConnectionState.Connected)?.takeIf { it.headUnitId == id }?.info?.restartNeeded == RestartKind.HMI,
            )
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), PairRequestUiState())

    /**
     * Takes the request the activity was started with, or sent on to it. The same code again changes nothing: it
     * is the same request, given again by a rotation, a restart of the process or the other app. A different one
     * takes the place of the one there was, and the screen goes back to its prompt.
     */
    fun handle(next: PairRequest) {
        if (next.code == code.value) return
        request++
        savedStateHandle[CODE] = next.code
        savedStateHandle[DEADLINE] = next.expiresInMs?.let { clock.millis() + it }
        step.value = PairRequestStep.Prompt
    }

    /** Ticks or clears the box that has the car set up to start the agent with it. Only on the prompt, where it is shown. */
    fun setEnableAutostart(on: Boolean) {
        if (step.value != PairRequestStep.Prompt) return
        savedStateHandle[ENABLE_AUTOSTART] = on
    }

    /** Runs the set-up again from the top, after it stopped short. */
    fun retrySetup() = setup.retry()

    /** Asks the agent to restart iDrive, once the set-up is done: see [AutostartSetup.restart]. */
    fun restartIdrive() = setup.restart()

    /** The user picks the car, among several saved. */
    fun choose(headUnitId: String) {
        if (step.value != PairRequestStep.Prompt) return
        savedStateHandle[PICKED] = headUnitId
    }

    /**
     * The user added a car on this screen, for this code: it is chosen, and pairing goes on with it at once. It is
     * saved a moment after it was added, which is waited for.
     */
    fun added(headUnitId: String) {
        if (step.value != PairRequestStep.Prompt) return
        savedStateHandle[PICKED] = headUnitId
        val mine = request
        step.value = PairRequestStep.Connecting
        viewModelScope.launch {
            val there = withTimeoutOrNull(SAVE_WAIT_MS) { saved.first { units -> units.orEmpty().any { it.id == headUnitId } } }
            if (mine != request) return@launch
            if (there == null) {
                step.value = PairRequestStep.NoLink("The car couldn't be saved")
                return@launch
            }
            step.value = PairRequestStep.Prompt
            pair()
        }
    }

    /**
     * Pair, and Retry after a failed connect: pairs the phone with the chosen car. A link to it that waits for a
     * code takes the code; a link on which the agent takes the phone means it is paired already. Otherwise a link
     * is opened first, which replaces whatever link the app has.
     *
     * The connect belongs to this screen: leaving it gives the attempt up, as [JobRepository.connect] says. The
     * code, once it goes, belongs to the app: see [send].
     */
    fun pair() {
        val unit = chosenHeadUnit(saved.value.orEmpty(), picked.value, jobs.connection.value) ?: return
        if (code.value == null) return
        if (step.value != PairRequestStep.Prompt && step.value !is PairRequestStep.NoLink) return
        if (timeUp()) {
            step.value = PairRequestStep.TimeUp
            return
        }
        // Kept, so the car does not change under the user if the link comes to refer to another.
        savedStateHandle[PICKED] = unit.id
        val mine = request
        val connection = jobs.connection.value
        when {
            connection is ConnectionState.Connected && connection.headUnitId == unit.id -> paired(PairRequestStep.AlreadyPaired(unit.name), unit)
            connection is ConnectionState.NeedsPairing && connection.headUnitId == unit.id -> send(unit, mine)
            else -> {
                step.value = PairRequestStep.Connecting
                viewModelScope.launch {
                    val result = jobs.connect(unit.id)
                    if (mine != request) return@launch
                    val failure = result.exceptionOrNull()
                    when {
                        failure == null -> paired(PairRequestStep.AlreadyPaired(unit.name), unit)
                        failure is PairingNeeded -> send(unit, mine)
                        else -> step.value = PairRequestStep.NoLink(failure.message ?: "The head unit couldn't be reached")
                    }
                }
            }
        }
    }

    /**
     * Hands the code to the agent over the link that waits for it, unless the code has run out. The attempt
     * belongs to the app, not to this screen, as in the pairing screen's: once the agent has issued the phone a
     * key, the key has to be kept and proved whether or not the user is still looking.
     */
    private fun send(unit: HeadUnit, mine: Int) {
        val code = code.value ?: return
        if (timeUp()) {
            step.value = PairRequestStep.TimeUp
            return
        }
        step.value = PairRequestStep.Pairing
        appScope.launch {
            val result = jobs.pair(code)
            if (mine != request) return@launch
            when (val failure = result.exceptionOrNull()) {
                null -> paired(PairRequestStep.Paired(unit.name), unit)
                is AgentRefusal -> step.value = PairRequestStep.Refused(pairRequestRefusal(failure.code, unit.name))
                else -> step.value = PairRequestStep.NoLink(failure.message ?: "The phone couldn't be paired")
            }
        }
    }

    /**
     * The phone is paired with [unit], as [done] says: now, or from before. With the box ticked the car is set up to
     * start the agent with it, before the step is shown, so the screen that stays for it has the set-up to show.
     */
    private fun paired(done: PairRequestStep, unit: HeadUnit) {
        // A set-up from an earlier pairing says nothing of this one.
        if (enableAutostart.value) setup.begin(unit.id) else setup.dismiss(unit.id)
        step.value = done
    }

    private fun timeUp(): Boolean = deadline.value?.let { clock.millis() >= it } ?: false

    private companion object {
        const val CODE = "code"

        /** When the code runs out, on [ElapsedClock]. */
        const val DEADLINE = "deadline"

        /** The car the user picked, or the one Pair was tapped for. */
        const val PICKED = "picked"

        /** Whether the box that has the car start the agent with it is ticked. */
        const val ENABLE_AUTOSTART = "enableAutostart"

        /** How long a car added here is waited for to be saved. */
        const val SAVE_WAIT_MS = 10_000L
    }
}

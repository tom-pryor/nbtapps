// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.PairingNeeded
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.reachesTcp
import com.nbtapps.store.data.saveCar
import com.nbtapps.store.di.ApplicationScope
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.ui.headunits.CheckFixFlow
import com.nbtapps.store.ui.headunits.CheckFixStep
import com.nbtapps.store.ui.headunits.ConnectGate
import com.nbtapps.store.ui.headunits.ConnectGuard
import dagger.hilt.android.lifecycle.HiltViewModel
import java.io.IOException
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
    /**
     * The saved car the code goes to: the user's choice, or the one there is to choose. Null while none is chosen,
     * and while [picking], where the car is [device] instead.
     */
    val chosen: HeadUnit? = null,
    /**
     * The car is picked from the devices paired with the phone: with no head unit saved, or once the user asked for
     * a different car than the saved ones.
     */
    val picking: Boolean = false,
    /** The device picked, while [picking]: the car the code goes to. Nothing is saved for it until Pair. */
    val device: PairedDevice? = null,
    /**
     * [device] was picked for the user, before the user picked anything, and is the car this phone is connected to
     * now. False where the user picked it, and where it is not connected.
     */
    val prePicked: Boolean = false,
    /**
     * The car the code goes to is new to the app: on the prompt, no head unit is saved with [device]'s address, and
     * Pair adds one; from Pair on, this request added the head unit [chosen].
     */
    val newCar: Boolean = false,
    /** This build reaches a head unit at a network address, so the prompt offers to add one by it. */
    val takesAddress: Boolean = false,
    /** How long the code is still good for, in milliseconds, or null if the other app did not say. */
    val remainingMs: Long? = null,
    val step: PairRequestStep = PairRequestStep.Prompt,
    /**
     * The box that has the car set up to start the agent with it once the phone is paired, ticked to begin with.
     * With it ticked the screen stays once the phone is paired, to show how that goes.
     */
    val enableAutostart: Boolean = true,
    /** The set-up for the chosen car, if there is one, with the stages it went through: see [AutostartSetup]. */
    val setupRun: AutostartSetupState? = null,
    /** The agent says over the link to the chosen car that iDrive has to restart. */
    val restartNeeded: Boolean = false,
    /** How many problems the agent's checks found on the chosen car that are not put aside, as is kept with it. */
    val openChecks: Int = 0,
    /** The confirmation of what the set-up asks to remove, while it is open. */
    val fix: CheckFixStep? = null,
) {
    /** Where the set-up for the chosen car stands, if there is one. */
    val setup: AutostartStep? get() = setupRun?.step

    /** The name of the car the code goes to: [device]'s while [picking], else [chosen]'s. Null with neither. */
    val carName: String? get() = if (picking) device?.name else chosen?.name

    /**
     * The line that leads to the car's page for what its checks found: once the phone is paired, where no set-up
     * runs or is shown, which would ask about them itself.
     */
    val showsChecks: Boolean
        get() = (step is PairRequestStep.Paired || step is PairRequestStep.AlreadyPaired) && setup == null && openChecks > 0

    val canPair: Boolean get() = step == PairRequestStep.Prompt && carName != null && code != null

    /** A failed connect can be tried again while the code is still good. */
    val canRetry: Boolean get() = step is PairRequestStep.NoLink && carName != null && (remainingMs == null || remainingMs > 0)
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

/** Whether [unit] has a Bluetooth serial endpoint to [device]'s address, however its letters are written. */
private fun isDevice(unit: HeadUnit, device: PairedDevice): Boolean =
    unit.endpoints.filterIsInstance<SerialEndpoint>().any { it.address.equals(device.address, ignoreCase = true) }

/**
 * Pairs this phone with a head unit by a code that another app handed over: see docs/pairing-code-handover.md. The
 * user chooses the car among the saved ones, or picks it from the devices paired with the phone, and on one tap of
 * Pair the app saves a car it did not have ([saveCar]), connects to it and sends the code over that link, which
 * the agent holds open waiting for it ([ConnectionState.NeedsPairing]). A phone the agent already takes is paired
 * already, and nothing is sent.
 *
 * A box, ticked to begin with, has the car set up to start the agent with it ([AutostartSetup]) once the phone is
 * paired, now or from before. Nothing is known of the agent before Pair, so the box is always shown.
 *
 * The code and its deadline are kept in the saved state, so a rotation or a restart of the process neither loses
 * the request nor starts its countdown again. So is the car chosen or picked.
 *
 * In a build that reaches a head unit at a network address ([reachesTcp]) the add dialog can add one by it
 * ([added]); elsewhere the car is always a saved one or one of the phone's paired devices.
 */
@HiltViewModel
class PairRequestViewModel @Inject constructor(
    private val savedStateHandle: SavedStateHandle,
    private val headUnits: HeadUnitRepository,
    private val jobs: JobRepository,
    private val setup: AutostartSetup,
    private val phone: PhoneBluetooth,
    connector: LinkConnector,
    @ApplicationScope private val appScope: CoroutineScope,
    private val clock: ElapsedClock,
    /** What the user's connect goes through first: [ConnectGate]. */
    private val guard: ConnectGuard = ConnectGuard.NONE,
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

    private val takesAddress = connector.reachesTcp

    /** What was picked among the phone's paired devices, and what this request added. */
    private data class Pick(
        /** The user asked for a different car than the saved ones, or a device was picked. */
        val picking: Boolean,
        val device: PairedDevice?,
        /** The picker picked [device] before the user picked anything. */
        val byDefault: Boolean,
        /** The head unit this request added, on Pair or by the add dialog. */
        val added: String?,
    )

    private val pick: Flow<Pick> = combine(
        savedStateHandle.getStateFlow(PICKING, false),
        savedStateHandle.getStateFlow<String?>(DEVICE_NAME, null),
        savedStateHandle.getStateFlow<String?>(DEVICE_ADDRESS, null),
        savedStateHandle.getStateFlow(BY_DEFAULT, false),
        savedStateHandle.getStateFlow<String?>(ADDED, null),
    ) { picking, name, address, byDefault, added ->
        Pick(picking, address?.let { PairedDevice(name ?: it, it) }, byDefault, added)
    }

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

    /** The saved head units, the one chosen among them, and what was picked. */
    private data class Target(val units: List<HeadUnit>, val chosen: HeadUnit?, val pick: Pick)

    /** Null until the saved head units have been read. */
    private val target = combine(saved, picked, jobs.connection, pick) { units, picked, connection, pick ->
        units?.let { Target(it, chosenHeadUnit(it, picked, connection), pick) }
    }

    private val pairing: Flow<PairRequestUiState> =
        combine(target, code, deadline, now, step) { target, code, deadline, now, step ->
            val remaining = if (deadline != null && now != null) (deadline - now).coerceAtLeast(0L) else null
            val units = target?.units.orEmpty()
            // With nothing saved there is nothing else to choose from.
            val picking = target != null && (target.pick.picking || units.isEmpty())
            val device = target?.pick?.device?.takeIf { picking }
            val chosen = target?.chosen?.takeUnless { picking }
            PairRequestUiState(
                loaded = target != null,
                code = code,
                headUnits = units,
                chosen = chosen,
                picking = picking,
                device = device,
                prePicked = device != null && target?.pick?.byDefault == true,
                newCar = if (device != null) units.none { isDevice(it, device) } else chosen != null && chosen.id == target?.pick?.added,
                takesAddress = takesAddress,
                remainingMs = remaining,
                // The code runs out while the user looks at it: there is nothing to tap Pair for then.
                step = if (step == PairRequestStep.Prompt && remaining == 0L) PairRequestStep.TimeUp else step,
            )
        }

    /** The confirmation of what the set-up asks to remove: the set-up runs the fix once the user has said yes. */
    private val fixes = CheckFixFlow(jobs, appScope)

    private val withSetup: Flow<PairRequestUiState> =
        combine(pairing, enableAutostart, setup.state, jobs.connection, fixes.state) { state, enable, setup, connection, fix ->
            val id = state.chosen?.id
            state.copy(
                enableAutostart = enable,
                setupRun = setup?.takeIf { it.headUnitId == id },
                restartNeeded = (connection as? ConnectionState.Connected)?.takeIf { it.headUnitId == id }?.info?.restartNeeded == RestartKind.HMI,
                openChecks = state.chosen?.checks?.open?.size ?: 0,
                fix = fix,
            )
        }

    val uiState: StateFlow<PairRequestUiState> =
        combine(withSetup, phone.connected) { state, connected ->
            val device = state.device
            state.copy(prePicked = state.prePicked && device != null && connected.any { it.equals(device.address, ignoreCase = true) })
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), PairRequestUiState())

    init {
        // Which devices are connected now, for the one picked for the user.
        phone.refreshConnected()
    }

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
        // A car an earlier request added is one the app had already for this one.
        savedStateHandle[ADDED] = null
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

    /** The fix's button on the set-up's card: what it moves aside and removes, to be confirmed. */
    fun openRemoval() {
        val id = chosenHeadUnit(saved.value.orEmpty(), picked.value, jobs.connection.value)?.id ?: return
        fixes.openForSetup(setup, id)
    }

    /** Skip on the set-up's card: see [AutostartSetup.skipRemoval]. */
    fun skipRemoval() = setup.skipRemoval()

    fun confirmRemoval() = fixes.confirm()

    fun retryRemoval() = fixes.retry()

    fun closeRemoval() = fixes.close()

    /** The user picks the car among the saved ones. A device picked before is put aside. */
    fun choose(headUnitId: String) {
        if (step.value != PairRequestStep.Prompt) return
        savedStateHandle[PICKED] = headUnitId
        clearPick()
    }

    /** "A different car…": the car is to be picked from the devices paired with the phone, not among the saved ones. */
    fun pickDifferent() {
        if (step.value != PairRequestStep.Prompt) return
        savedStateHandle[PICKING] = true
    }

    /**
     * The car picker picked [device]: the user did, or, [byDefault], the picker did before the user picked anything
     * ([CarPickerFlow][com.nbtapps.store.ui.headunits.CarPickerFlow]). Nothing is saved for it until Pair.
     */
    fun pickDevice(device: PairedDevice, byDefault: Boolean = false) {
        if (step.value != PairRequestStep.Prompt) return
        savedStateHandle[PICKING] = true
        savedStateHandle[DEVICE_NAME] = device.name
        savedStateHandle[DEVICE_ADDRESS] = device.address
        savedStateHandle[BY_DEFAULT] = byDefault
    }

    private fun clearPick() {
        savedStateHandle[PICKING] = false
        savedStateHandle[DEVICE_NAME] = null
        savedStateHandle[DEVICE_ADDRESS] = null
        savedStateHandle[BY_DEFAULT] = false
    }

    /**
     * The add dialog, in a build that reaches a head unit at a network address, added a car on this screen for this
     * code, or found it saved: it is chosen, and pairing goes on with it at once. It is saved a moment after it was
     * added, which is waited for.
     */
    fun added(headUnitId: String) {
        if (step.value != PairRequestStep.Prompt) return
        if (saved.value.orEmpty().none { it.id == headUnitId }) savedStateHandle[ADDED] = headUnitId
        savedStateHandle[PICKED] = headUnitId
        clearPick()
        val mine = request
        step.value = PairRequestStep.Connecting
        viewModelScope.launch {
            if (!awaitSaved(headUnitId, mine)) return@launch
            step.value = PairRequestStep.Prompt
            pair()
        }
    }

    /**
     * Waits for the head unit [headUnitId] to be in the saved list as read here. True once it is, unless the request
     * [mine] was replaced meanwhile. Where it does not come, the screen says the car couldn't be saved.
     */
    private suspend fun awaitSaved(headUnitId: String, mine: Int): Boolean {
        val there = withTimeoutOrNull(SAVE_WAIT_MS) { saved.first { units -> units.orEmpty().any { it.id == headUnitId } } }
        if (mine != request) return false
        if (there == null) step.value = PairRequestStep.NoLink(NOT_SAVED)
        return there != null
    }

    /**
     * Pair, and Retry after a failed connect: pairs the phone with the chosen car. A device picked is saved first
     * ([saveAndPair]). A link to the car that waits for a code takes the code; a link on which the agent takes the
     * phone means it is paired already. Otherwise a link is opened first, which replaces whatever link the app has.
     *
     * The connect belongs to this screen: leaving it gives the attempt up, as [JobRepository.connect] says. The
     * code, once it goes, belongs to the app: see [send].
     */
    fun pair() {
        if (picking()) return saveAndPair()
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
                viewModelScope.launch {
                    // A terminal or the SSH port of another head unit is not ended unasked; on Cancel nothing changes.
                    if (!guard.mayConnect(unit.id) || mine != request) return@launch
                    step.value = PairRequestStep.Connecting
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

    /** Whether the car is picked from the phone's paired devices, as [PairRequestUiState.picking] says. */
    private fun picking(): Boolean = savedStateHandle.get<Boolean>(PICKING) == true || saved.value?.isEmpty() == true

    /** The device the code goes to, where the car is picked from the phone's paired devices and one is picked. */
    private fun pickedDevice(): PairedDevice? {
        if (!picking()) return null
        val address = savedStateHandle.get<String>(DEVICE_ADDRESS) ?: return null
        return PairedDevice(savedStateHandle.get<String>(DEVICE_NAME) ?: address, address)
    }

    /**
     * Pair with a device picked: it is saved ([saveCar]), unless a head unit is saved with its address already, which
     * is used as it is. That head unit is then the chosen car, as a saved one is, and pairing goes on with it as
     * [pair] does. A code that has run out saves nothing.
     */
    private fun saveAndPair() {
        val device = pickedDevice() ?: return
        if (code.value == null) return
        if (step.value != PairRequestStep.Prompt && step.value !is PairRequestStep.NoLink) return
        if (timeUp()) {
            step.value = PairRequestStep.TimeUp
            return
        }
        val mine = request
        step.value = PairRequestStep.Connecting
        viewModelScope.launch {
            val id = try {
                val before = headUnits.headUnits.first().map { it.id }.toSet()
                headUnits.saveCar(device).also { if (it !in before) savedStateHandle[ADDED] = it }
            } catch (e: IOException) {
                if (mine == request) step.value = PairRequestStep.NoLink(e.message ?: NOT_SAVED)
                return@launch
            }
            if (!awaitSaved(id, mine)) return@launch
            savedStateHandle[PICKED] = id
            clearPick()
            step.value = PairRequestStep.Prompt
            pair()
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

        /** Whether the car is picked from the phone's paired devices: see [Pick.picking]. */
        const val PICKING = "picking"

        /** The device picked: its name and address. */
        const val DEVICE_NAME = "deviceName"
        const val DEVICE_ADDRESS = "deviceAddress"

        /** Whether the picker picked the device before the user did. */
        const val BY_DEFAULT = "byDefault"

        /** The head unit this request added. */
        const val ADDED = "added"

        /** How long a car added here is waited for to be saved. */
        const val SAVE_WAIT_MS = 10_000L

        const val NOT_SAVED = "The car couldn't be saved"
    }
}

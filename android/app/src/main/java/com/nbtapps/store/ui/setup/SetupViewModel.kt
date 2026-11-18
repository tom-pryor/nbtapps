// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.data.SetupRepository
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.headUnitId
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.SerialDiagnostics
import com.nbtapps.store.data.link.reachesTcp
import com.nbtapps.store.data.saveCar
import com.nbtapps.store.di.ApplicationScope
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.ui.headunits.CheckFixFlow
import com.nbtapps.store.ui.headunits.CheckFixStep
import com.nbtapps.store.ui.pairing.CodeEntry
import com.nbtapps.store.ui.pairing.CodePairing
import dagger.hilt.android.lifecycle.HiltViewModel
import java.io.IOException
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.updateAndGet
import kotlinx.coroutines.launch

/** What the set-up shows: the page ([step]) and what its pages need. */
data class SetupUiState(
    val step: SetupStep = SetupStep.Welcome,
    /** The set-up adds a further car: it begins at [SetupStep.PickCar], with no question before it. */
    val furtherCar: Boolean = false,
    /** How many numbered steps the header counts: see [setupStepCount]. */
    val stepCount: Int = 4,
    /** This app's id of the head unit being set up, once a car is picked. */
    val headUnitId: String? = null,
    /** The head unit's name, once a car is picked. */
    val name: String = "",
    /** The device picked in the list of the pick-the-car page, before Connect is tapped. */
    val selected: PairedDevice? = null,
    /** The car picked is being saved. */
    val saving: Boolean = false,
    /** Why the car picked could not be saved. */
    val saveError: String? = null,
    /** This build reaches a head unit at a network address, so the pick-the-car page offers to add one by it. */
    val takesAddress: Boolean = false,
    /**
     * Whether the car has a hands-free or audio connection to the phone now ([PhoneBluetooth.connected]). Null for a
     * head unit with no Bluetooth endpoint.
     */
    val carInReach: Boolean? = null,
    /** The code from the car's screen as typed, and how handing it over goes. */
    val code: CodeEntry = CodeEntry(),
    /** The box on the Pair page that has the car set up to start the agent with it. Ticked to begin with. */
    val enableAutostart: Boolean = true,
    /** The set-up to start the agent with the car, for this head unit only. */
    val setup: AutostartSetupState? = null,
    /** The agent says over the link to this head unit that iDrive has to restart. */
    val restartNeeded: Boolean = false,
    /** The agent on this head unit was last seen switched to start with the car. */
    val startsWithCar: Boolean = false,
    /** The confirmation of what the set-up asks to remove, while it is open. */
    val fix: CheckFixStep? = null,
)

/**
 * The set-up that walks a new user through adding the car, connecting to the agent on it, pairing the phone and
 * having the agent start with the car: see [setupStep] for which page it shows when.
 *
 * What the user answered and what has been seen of the car are kept in the saved state, so a rotation or a restart
 * of the process finds the same page. The page follows the app's own state as well as the user's answers: the link
 * and the set-up to start with the car are the app's, and another app's pairing code may pair the same car while
 * this is open.
 *
 * The connect to the car is this set-up's own, and goes when it is left, as [JobRepository.connect] says; a car that
 * the set-up added, and that no agent ever answered on, is removed again when the user gives it up ([cancelConnect],
 * [pickAnother]), so a wrong pick leaves nothing behind. The code, once it goes, and the set-up to start with the car
 * are the app's, as on the pairing screen.
 */
@HiltViewModel
class SetupViewModel @Inject constructor(
    private val savedStateHandle: SavedStateHandle,
    private val headUnits: HeadUnitRepository,
    private val jobs: JobRepository,
    private val setup: AutostartSetup,
    private val phone: PhoneBluetooth,
    private val setupChoice: SetupRepository,
    connector: LinkConnector,
    @ApplicationScope private val appScope: CoroutineScope,
    private val diagnostics: SerialDiagnostics = SerialDiagnostics(),
) : ViewModel() {

    /** Whether the set-up adds a further car: the route says so. */
    val furtherCar: Boolean = savedStateHandle.get<Boolean>(FURTHER_CAR) ?: false

    private val takesAddress = connector.reachesTcp

    /** What is kept in the saved state, as one value: see [read] and [change]. */
    private data class Kept(
        val answers: SetupAnswers,
        /** The set-up added the head unit itself: it was not saved before. */
        val added: Boolean = false,
        /** An agent answered on the head unit: it waited for the code, or took the phone. */
        val answered: Boolean = false,
        /** The head unit's name, as it was when picked: shown until the saved list has been read. */
        val name: String? = null,
        /** The car's Bluetooth address, as it was when picked. */
        val address: String? = null,
        /** The device picked in the list, before Connect: its name and address. */
        val selectedName: String? = null,
        val selectedAddress: String? = null,
        /** The agent on the head unit was last seen switched to start with the car. */
        val startsWithCar: Boolean = false,
    )

    private val kept = MutableStateFlow(read())

    /** What [pickCar] is doing: saving the car, or why it could not. */
    private data class Saving(val running: Boolean = false, val error: String? = null)

    private val saving = MutableStateFlow(Saving())

    /** The set-up's own connect to the car, while it runs. */
    @Volatile
    private var attempt: Job? = null

    private val attempting = MutableStateFlow(false)

    /** The code as typed, and handing it to the agent, on the app's scope. */
    private val codes = CodePairing(jobs, setup, appScope)

    /** The confirmation of what the set-up asks to remove: the set-up runs the fix once the user has said yes. */
    private val fixes = CheckFixFlow(jobs, appScope)

    /** The ids saved when the add dialog was opened, to tell a head unit it adds from one it finds saved. */
    @Volatile
    private var savedBeforeAdd: Set<String>? = null

    private data class Position(
        val kept: Kept,
        val connection: ConnectionState,
        val setup: AutostartSetupState?,
        val connecting: Boolean,
        val code: CodeEntry,
    )

    private val position = combine(kept, jobs.connection, setup.state, attempting, codes.state, ::Position)

    val uiState: StateFlow<SetupUiState> =
        combine(position, headUnits.headUnits, phone.connected, saving, fixes.state) { at, units, connected, saving, fix ->
            uiState(at, units, connected, saving, fix)
        }.stateIn(
            viewModelScope,
            SharingStarted.WhileSubscribed(5_000),
            uiState(Position(kept.value, jobs.connection.value, setup.state.value, attempting.value, codes.state.value), null, emptySet(), Saving(), null),
        )

    init {
        phone.refreshConnected()
        // A link that is up as the set-up opens was not made while it was open: see adopt.
        val atStart = jobs.connection.value
        // What is seen of the car picked is kept: that an agent answered on it, and that it takes the phone.
        viewModelScope.launch {
            combine(kept, jobs.connection) { kept, connection -> kept.answers.headUnitId to connection }.collect { (id, connection) ->
                if (id == null && connection is ConnectionState.Connected && connection != atStart) adopt(connection)
                if (id == null || connection.headUnitId != id) return@collect
                when (connection) {
                    is ConnectionState.NeedsPairing -> change(id) {
                        it.copy(answered = true, answers = it.answers.copy(offersAutostart = connection.offersAutostart))
                    }
                    is ConnectionState.Connected -> change(id) {
                        it.copy(answered = true, startsWithCar = connection.autostart?.on == true, answers = it.answers.copy(paired = true))
                    }
                    else -> Unit
                }
            }
        }
        // A process that was stopped while the set-up connected: the connect goes on where it was.
        val restored = kept.value.answers
        if (restored.headUnitId != null && !restored.paired && jobs.connection.value.headUnitId != restored.headUnitId) {
            connect(restored.headUnitId)
        }
    }

    private fun uiState(at: Position, units: List<HeadUnit>?, connected: Set<String>, saving: Saving, fix: CheckFixStep?): SetupUiState {
        val answers = at.kept.answers
        val id = answers.headUnitId
        val unit = units?.firstOrNull { it.id == id }
        val own = at.connection.takeIf { id != null && it.headUnitId == id }
        val address = unit?.let { it.endpoints.filterIsInstance<SerialEndpoint>().firstOrNull()?.address } ?: at.kept.address.takeIf { unit == null }
        return SetupUiState(
            step = setupStep(answers, at.connection, at.setup, at.connecting, at.code.working),
            furtherCar = furtherCar,
            stepCount = setupStepCount(answers),
            headUnitId = id,
            name = unit?.name ?: at.kept.name ?: "",
            selected = at.kept.selectedAddress?.let { PairedDevice(at.kept.selectedName ?: it, it) },
            saving = saving.running,
            saveError = saving.error,
            takesAddress = takesAddress,
            carInReach = address?.let { car -> connected.any { it.equals(car, ignoreCase = true) } },
            code = at.code,
            enableAutostart = answers.enableAutostart,
            setup = at.setup?.takeIf { it.headUnitId == id },
            restartNeeded = (own as? ConnectionState.Connected)?.info?.restartNeeded == RestartKind.HMI,
            startsWithCar = at.kept.startsWithCar,
            fix = fix,
        )
    }

    // ---- Welcome and Not yet ----

    /** The user's answer to whether the agent is on the car: true, false for "not yet", or null to ask again. */
    fun answer(installed: Boolean?) = change { it.copy(answers = it.answers.copy(agentInstalled = installed)) }

    /**
     * The user skips the set-up, from any page, or leaves it where that is the same: it is not offered at the next
     * start. The screen is left by the caller.
     */
    fun skip() {
        appScope.launch { setupChoice.setSkipped(true) }
    }

    // ---- Pick the car ----

    /** The user picks [device] in the list. Nothing is saved until Connect is tapped ([pickCar]). */
    fun select(device: PairedDevice) = change { it.copy(selectedName = device.name, selectedAddress = device.address) }

    /**
     * Connect on the pick-the-car page: saves the car picked, unless it is saved already ([saveCar]), and connects
     * to it. A link to it that is there already is used as it is.
     */
    fun pickCar() {
        val picked = kept.value
        val device = picked.selectedAddress?.let { PairedDevice(picked.selectedName ?: it, it) } ?: return
        if (picked.answers.headUnitId != null || saving.value.running) return
        saving.value = Saving(running = true)
        viewModelScope.launch {
            try {
                val before = headUnits.headUnits.first().map { it.id }.toSet()
                val id = headUnits.saveCar(device)
                saving.value = Saving()
                choose(id, added = id !in before, name = device.name, address = device.address)
            } catch (e: IOException) {
                saving.value = Saving(error = e.message ?: "The car couldn't be saved")
            }
        }
    }

    /** The add dialog is opened, in a build that reaches a head unit at a network address: see [addedByAddress]. */
    fun beginAddress() {
        viewModelScope.launch { savedBeforeAdd = headUnits.headUnits.first().map { it.id }.toSet() }
    }

    /**
     * The add dialog saved the head unit [headUnitId], or found it saved: it is the car the set-up goes on with,
     * and it is connected to.
     */
    fun addedByAddress(headUnitId: String) {
        if (kept.value.answers.headUnitId != null) return
        val before = savedBeforeAdd
        savedBeforeAdd = null
        choose(headUnitId, added = before != null && headUnitId !in before, name = null, address = null)
    }

    /**
     * The app's link came up, [connected], while the set-up for a new user had no car yet: on Welcome, Not yet or
     * pick-the-car. The phone was paired meanwhile from outside, as by a code another app handed over, so the head
     * unit linked to is the car being set up, already paired: the set-up goes on to start-with-the-car while there
     * is a set-up for it that has not been left, and to Done otherwise ([setupStep]). The set-up did not add it, so
     * giving it up removes nothing. A set-up for a further car is the user's to point at a car, and is not moved.
     */
    private fun adopt(connected: ConnectionState.Connected) {
        if (furtherCar) return
        change {
            if (it.answers.headUnitId != null) {
                it
            } else {
                Kept(
                    answers = SetupAnswers(
                        agentInstalled = true,
                        headUnitId = connected.headUnitId,
                        paired = true,
                        enableAutostart = it.answers.enableAutostart,
                    ),
                    answered = true,
                    startsWithCar = connected.autostart?.on == true,
                    selectedName = it.selectedName,
                    selectedAddress = it.selectedAddress,
                )
            }
        }
    }

    /** Takes [id] as the car being set up, and connects to it unless the app has a link to it already. */
    private fun choose(id: String, added: Boolean, name: String?, address: String?) {
        val own = jobs.connection.value.takeIf { it.headUnitId == id }
        if (own !is ConnectionState.Connected && own !is ConnectionState.NeedsPairing && own !is ConnectionState.Connecting) connect(id)
        change {
            Kept(
                answers = SetupAnswers(agentInstalled = true, headUnitId = id, enableAutostart = it.answers.enableAutostart),
                added = added,
                name = name,
                address = address,
                selectedName = it.selectedName,
                selectedAddress = it.selectedAddress,
            )
        }
    }

    // ---- Connect ----

    /**
     * Opens the link to the head unit [id], in place of the set-up's last try. The try belongs to this set-up: it
     * is given up when the set-up is left, as [JobRepository.connect] says.
     */
    private fun connect(id: String) {
        attempt?.cancel()
        val job = viewModelScope.launch(start = CoroutineStart.LAZY) { jobs.connect(id) }
        attempt = job
        attempting.value = true
        job.invokeOnCompletion { if (attempt === job) attempting.value = false }
        job.start()
    }

    /** What "Copy diagnostics" copies, where the connect failed: see [SerialDiagnostics.report]. */
    fun diagnostics(): String {
        val headUnitId = kept.value.answers.headUnitId
        return diagnostics.report(uiState.value.name.ifEmpty { null }, headUnitId?.let(jobs::agentSerial))
    }

    /** Try again, after the connect failed. */
    fun retry() {
        kept.value.answers.headUnitId?.let(::connect)
    }

    /** Cancel on the Connect page, and back from it: the connect is given up, and the car picked with it. */
    fun cancelConnect() = giveUpCar()

    /** Pick another car, after the connect failed: the car picked is given up. */
    fun pickAnother() = giveUpCar()

    /**
     * Gives up the set-up's connect and the car picked, and goes back to picking one. A car the set-up added itself,
     * on which no agent ever answered, is removed again; one that was saved before, or that an agent answered on,
     * stays.
     */
    private fun giveUpCar() {
        attempt?.cancel()
        attempt = null
        attempting.value = false
        forgetCar(removeUnanswered = true)
    }

    /** Back from the Pair page: the link is ended, and the car is no longer the one being set up. It stays saved. */
    fun backFromPair() {
        appScope.launch { jobs.disconnect() }
        forgetCar(removeUnanswered = false)
    }

    private fun forgetCar(removeUnanswered: Boolean) {
        val before = kept.value
        val id = before.answers.headUnitId ?: return
        change {
            if (it.answers.headUnitId != id) {
                it
            } else {
                Kept(
                    answers = SetupAnswers(agentInstalled = true, enableAutostart = it.answers.enableAutostart),
                    selectedName = it.selectedName,
                    selectedAddress = it.selectedAddress,
                )
            }
        }
        if (removeUnanswered && before.added && !before.answered) appScope.launch { headUnits.remove(id) }
    }

    /** Reads the phone's Bluetooth connections again: on return to the screen. */
    fun refreshBluetooth() = phone.refreshConnected()

    // ---- Pair ----

    fun setCode(typed: String) = codes.setCode(typed)

    /** Ticks or clears the box that has the car set up to start the agent with it once the phone is paired. */
    fun setEnableAutostart(on: Boolean) = change { it.copy(answers = it.answers.copy(enableAutostart = on)) }

    /**
     * Hands the code to the agent over the link that waits for it, with the box as it stands where the agent
     * offers to start with the car. The hand-over belongs to the app: see [CodePairing].
     */
    fun pair() {
        val id = kept.value.answers.headUnitId ?: return
        val waiting = (jobs.connection.value as? ConnectionState.NeedsPairing)?.takeIf { it.headUnitId == id } ?: return
        codes.pair(id, autostart = kept.value.answers.enableAutostart && waiting.offersAutostart)
    }

    // ---- Start with the car ----

    /** Runs the set-up again from the top, after it stopped short. */
    fun retrySetup() = setup.retry()

    /** Asks the agent to restart iDrive, once the set-up is done: see [AutostartSetup.restart]. */
    fun restartIdrive() = setup.restart()

    /** The fix's button in the checklist: what it moves aside and removes, to be confirmed. */
    fun openRemoval() {
        kept.value.answers.headUnitId?.let { fixes.openForSetup(setup, it) }
    }

    /** Skip in the checklist: see [AutostartSetup.skipRemoval]. */
    fun skipRemoval() = setup.skipRemoval()

    fun confirmRemoval() = fixes.confirm()

    fun retryRemoval() = fixes.retry()

    fun closeRemoval() = fixes.close()

    /** The user moves on from the start-with-the-car page. The set-up goes on by itself, if it still runs. */
    fun moveOn() = change { it.copy(answers = it.answers.copy(movedOn = true)) }

    // ---- Back ----

    /**
     * What back does on the page shown, the system's or the header's arrow. Returns true where it leaves the set-up,
     * which is the caller's to do: on Welcome, where it is "Just browse the catalogue", and on start-with-the-car,
     * both of which skip it; on pick-the-car for a further car; and on Done.
     */
    fun back(): Boolean = when (currentStep()) {
        SetupStep.Welcome, SetupStep.StartWithCar -> {
            skip()
            true
        }
        SetupStep.NotYet -> {
            answer(null)
            false
        }
        SetupStep.PickCar -> if (furtherCar) {
            true
        } else {
            answer(null)
            false
        }
        is SetupStep.Connect -> {
            cancelConnect()
            false
        }
        is SetupStep.Pair -> {
            // A code that is with the agent is let finish: the link it goes over is not ended under it.
            if (!codes.state.value.working) backFromPair()
            false
        }
        SetupStep.Done -> true
    }

    /** The page as things stand now, read afresh: [uiState] is kept up to date only while it is watched. */
    private fun currentStep(): SetupStep =
        setupStep(kept.value.answers, jobs.connection.value, setup.state.value, attempting.value, codes.state.value.working)

    // ---- The saved state ----

    private fun read(): Kept = Kept(
        answers = SetupAnswers(
            agentInstalled = savedStateHandle.get<Boolean>(AGENT_INSTALLED) ?: true.takeIf { furtherCar },
            headUnitId = savedStateHandle[HEAD_UNIT],
            paired = savedStateHandle[PAIRED] ?: false,
            movedOn = savedStateHandle[MOVED_ON] ?: false,
            offersAutostart = savedStateHandle[OFFERS_AUTOSTART],
            enableAutostart = savedStateHandle[ENABLE_AUTOSTART] ?: true,
        ),
        added = savedStateHandle[ADDED] ?: false,
        answered = savedStateHandle[ANSWERED] ?: false,
        name = savedStateHandle[NAME],
        address = savedStateHandle[ADDRESS],
        selectedName = savedStateHandle[SELECTED_NAME],
        selectedAddress = savedStateHandle[SELECTED_ADDRESS],
        startsWithCar = savedStateHandle[STARTS_WITH_CAR] ?: false,
    )

    /** Changes what is kept, in memory as one value and in the saved state. */
    private fun change(block: (Kept) -> Kept) {
        val next = synchronized(kept) {
            val before = kept.value
            val after = kept.updateAndGet(block)
            if (after == before) return
            after
        }
        write(next)
    }

    /** Changes what is kept, if the car being set up is still [id]. */
    private fun change(id: String, block: (Kept) -> Kept) = change { if (it.answers.headUnitId == id) block(it) else it }

    private fun write(kept: Kept) {
        val answers = kept.answers
        savedStateHandle[AGENT_INSTALLED] = answers.agentInstalled
        savedStateHandle[HEAD_UNIT] = answers.headUnitId
        savedStateHandle[PAIRED] = answers.paired
        savedStateHandle[MOVED_ON] = answers.movedOn
        savedStateHandle[OFFERS_AUTOSTART] = answers.offersAutostart
        savedStateHandle[ENABLE_AUTOSTART] = answers.enableAutostart
        savedStateHandle[ADDED] = kept.added
        savedStateHandle[ANSWERED] = kept.answered
        savedStateHandle[NAME] = kept.name
        savedStateHandle[ADDRESS] = kept.address
        savedStateHandle[SELECTED_NAME] = kept.selectedName
        savedStateHandle[SELECTED_ADDRESS] = kept.selectedAddress
        savedStateHandle[STARTS_WITH_CAR] = kept.startsWithCar
    }

    companion object {
        /** The route's argument: the set-up adds a further car. Its name is the route's property's. */
        const val FURTHER_CAR = "furtherCar"

        private const val AGENT_INSTALLED = "agentInstalled"
        private const val HEAD_UNIT = "headUnit"
        private const val PAIRED = "paired"
        private const val MOVED_ON = "movedOn"
        private const val OFFERS_AUTOSTART = "offersAutostart"
        private const val ENABLE_AUTOSTART = "enableAutostart"
        private const val ADDED = "added"
        private const val ANSWERED = "answered"
        private const val NAME = "name"
        private const val ADDRESS = "address"
        private const val SELECTED_NAME = "selectedName"
        private const val SELECTED_ADDRESS = "selectedAddress"
        private const val STARTS_WITH_CAR = "startsWithCar"
    }
}

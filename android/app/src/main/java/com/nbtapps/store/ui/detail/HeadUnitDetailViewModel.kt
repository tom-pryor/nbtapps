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
import com.nbtapps.store.data.PrereleaseRepository
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.jobs.SerialResetOutcome
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.SerialDiagnostics
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.data.serialUnavailable
import com.nbtapps.store.data.ssh.SshAccess
import com.nbtapps.store.data.ssh.SshEntry
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.ssh.sshOffered
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.di.ApplicationScope
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitChecks
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.ui.headunits.CheckFixFlow
import com.nbtapps.store.ui.headunits.CheckFixStep
import com.nbtapps.store.ui.headunits.ConnectGuard
import com.nbtapps.store.ui.navigation.HeadUnitDetailRoute
import com.nbtapps.store.ui.restart.RestartQuestion
import com.nbtapps.store.ui.restart.RestartQuestions
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOf
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
    /** [fromPhone]: the connect only waited for the car's link, and the phone may connect itself: see [ConnectionState.Failed.listenedOnly]. */
    data class Failed(val reason: String, val fromPhone: Boolean = false) : LinkState
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

/** Where the Bluetooth link's reset asked from the head unit's page stands: see [HeadUnitDetailViewModel.resetSerial]. */
sealed interface SerialResetUi {
    data object Working : SerialResetUi

    /** It is over: [text] says how it went, in one line, and [failed] where it could not be done. */
    data class Done(val text: String, val failed: Boolean = false) : SerialResetUi
}

/** What the page says of how the reset went: see [JobRepository.resetSerial]. */
internal fun serialResetText(result: Result<SerialResetOutcome>): SerialResetUi.Done {
    val outcome = result.getOrElse { return SerialResetUi.Done(it.message ?: "The head unit didn't answer", failed = true) }
    return SerialResetUi.Done(
        when (outcome) {
            is SerialResetOutcome.Armed ->
                "No Bluetooth link was up, so the car will end the next one this phone opens within ${within(outcome.forMs)}."
            is SerialResetOutcome.Sent -> when {
                outcome.endedByCar && outcome.connected -> "The car ended the Bluetooth link, and the app connected again."
                outcome.endedByCar -> "The car ended the Bluetooth link. The app couldn't connect again by itself."
                outcome.connected -> "The car didn't end the Bluetooth link in time, so the app ended it and connected again."
                else -> "The car didn't end the Bluetooth link in time, so the app ended it. It couldn't connect again by itself."
            }
        },
    )
}

/** [millis] as the outcome's line says it: a minute, as the agent has it, or so many seconds. */
private fun within(millis: Long): String = when {
    millis == 60_000L -> "a minute"
    millis % 60_000L == 0L -> "${millis / 60_000L} minutes"
    else -> "${millis / 1000L} seconds"
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
    /**
     * The ids of the [installed] packages whose installed version is on this phone with a manifest that uses features
     * this version of the app does not know ([StoredPackage.laterFormat]).
     */
    val laterFormat: Set<String> = emptySet(),
    /** The running or last job on this head unit. */
    val job: JobView? = null,
    /** Why the last ask to reboot failed: Restart iDrive says its own in its question ([HeadUnitDetailViewModel.restartQuestion]). */
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
    /**
     * There is a link to this head unit, and its agent gives and changes packages' settings: see
     * [com.nbtapps.store.ui.settings.SettingsScreen].
     */
    val settings: Boolean = false,
    /** The set-up that has this head unit start the agent with the car, begun on pairing, if there is one: see [AutostartSetup]. */
    val setup: AutostartSetupState? = null,
    /** The agent says over the link to this head unit that iDrive has to restart. */
    val restartNeeded: Boolean = false,
    /** What the agent's checks found on this head unit, as it is kept with it. Null until they have been answered. */
    val checks: HeadUnitChecks? = null,
    /** There is a link to this head unit, and its agent has the checks: they can be run and fixed from here. */
    val canCheck: Boolean = false,
    /** The checks are being run again by hand. */
    val checking: Boolean = false,
    /** Why running them again failed. */
    val checkError: String? = null,
    /** The confirmation of a check's fix that is open, if one is. */
    val fix: CheckFixStep? = null,
    /**
     * There is a link to this head unit, its agent can have the car end the phone's Bluetooth link, and no job needs
     * the phone: see [JobRepository.resetSerial].
     */
    val canResetSerial: Boolean = false,
    /** The reset asked from this page, while it runs and once it is over. Null where none was. */
    val serialReset: SerialResetUi? = null,
    /** There is a link to this head unit, and its agent has channels: see [DebugCard]. */
    val channels: Boolean = false,
    /** The version of the agent on this head unit, as it says over the link that is open, or said last. */
    val agentVersion: String? = null,
    /** The Debug card, where it is shown. */
    val debug: DebugCard? = null,
)

/**
 * The Debug card for [page], or null where it is not shown: with a link, where the agent has channels and the SSH
 * package is installed or [offered]; with none, where the package was installed when the agent last said, and then
 * it cannot be opened. [entry] is the side menu's entry for SSH, whose line the card's row shows where it is of
 * [headUnitId].
 */
internal fun debugCard(page: HeadUnitDetailUiState, offered: Boolean, entry: SshEntry, headUnitId: String): DebugCard? {
    val installed = page.installed.any { it.id == SshTunnel.PACKAGE }
    val connected = page.link is LinkState.Connected
    val shown = if (connected) page.channels && (installed || offered) else installed
    return when {
        !shown -> null
        !connected -> DebugCard.Apart
        !installed -> DebugCard.Install
        else -> DebugCard.Ready((entry as? SshEntry.Ready)?.takeIf { it.headUnitId == headUnitId }?.line)
    }
}

/** The ids of the [installed] packages whose installed version is among the [stored] archives with [StoredPackage.laterFormat]. */
internal fun laterFormat(installed: List<InstalledPackage>, stored: List<StoredPackage>): Set<String> =
    installed.filter { pkg -> stored.any { it.id == pkg.id && it.version == pkg.version && it.laterFormat } }.mapTo(HashSet()) { it.id }

@HiltViewModel
class HeadUnitDetailViewModel internal constructor(
    private val headUnitId: String,
    private val repository: HeadUnitRepository,
    private val jobs: JobRepository,
    private val connector: LinkConnector,
    private val bluetooth: PhoneBluetooth,
    private val setup: AutostartSetup,
    private val appScope: CoroutineScope,
    /** The archives on this phone, for what their manifests say: see [HeadUnitDetailUiState.laterFormat]. */
    private val stored: Flow<List<StoredPackage>> = flowOf(emptyList()),
    private val diagnostics: SerialDiagnostics = SerialDiagnostics(),
    /** The catalogue, for whether it offers the SSH package. */
    private val catalogue: Flow<CatalogueState> = flowOf(CatalogueState()),
    /** The packages whose pre-releases are offered. */
    private val switchedOn: Flow<Set<String>> = flowOf(emptySet()),
    /** The side menu's entry for SSH, whose line the Debug card's row shows: [SshAccess.entry]. */
    private val sshEntry: Flow<SshEntry> = flowOf(SshEntry.NotConnected),
    /** Has the SSH switch read again as the page opens: [SshAccess.refresh]. */
    refreshSsh: (headUnitId: String) -> Unit = {},
    /** What the user's connect goes through first: [com.nbtapps.store.ui.headunits.ConnectGate]. */
    private val guard: ConnectGuard = ConnectGuard.NONE,
    /** Makes the question before iDrive restarts: by default over [jobs]' link, with no terminal or port open. */
    restarts: RestartQuestions = RestartQuestions(
        jobs.connection,
        MutableStateFlow(TerminalState.Idle),
        MutableStateFlow(SshPortState.Off()),
        { id -> jobs.restart(id, "hmi") },
    ),
) : ViewModel() {

    @Inject
    constructor(
        savedStateHandle: SavedStateHandle,
        repository: HeadUnitRepository,
        jobs: JobRepository,
        connector: LinkConnector,
        bluetooth: PhoneBluetooth,
        setup: AutostartSetup,
        @ApplicationScope appScope: CoroutineScope,
        store: PackageStore,
        diagnostics: SerialDiagnostics,
        catalogue: CatalogueRepository,
        prereleases: PrereleaseRepository,
        ssh: SshAccess,
        guard: ConnectGuard,
        restarts: RestartQuestions,
    ) : this(
        savedStateHandle.toRoute<HeadUnitDetailRoute>().headUnitId, repository, jobs, connector, bluetooth, setup, appScope, store.packages,
        diagnostics, catalogue.catalogue, prereleases.switchedOn, ssh.entry, ssh::refresh, guard, restarts,
    )

    private data class Local(
        val serial: SerialStatus? = null,
        val restartError: String? = null,
        val autostartBusy: Boolean = false,
        val autostartError: String? = null,
        val checking: Boolean = false,
        val checkError: String? = null,
        val serialReset: SerialResetUi? = null,
    )

    private val local = MutableStateFlow(Local())

    /** The confirmation of a check's fix: it is the app's, so a fix goes on when the screen is left. */
    private val fixes = CheckFixFlow(jobs, appScope)

    /** The question before iDrive restarts on this head unit, which Restart iDrive asks ([askRestart]), with its answer and refusal. */
    val restartQuestion: RestartQuestion = restarts.question(viewModelScope)

    private val page =
        combine(repository.headUnit(headUnitId), jobs.connection, jobs.job, local, setup.state) { unit, connection, job, local, setup ->
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
                    connection is ConnectionState.Failed && connection.headUnitId == headUnitId -> LinkState.Failed(connection.reason, connection.listenedOnly)
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
                settings = connected?.settings == true,
                setup = setup?.takeIf { it.headUnitId == headUnitId },
                restartNeeded = connected?.info?.restartNeeded == RestartKind.HMI,
                checks = unit?.checks,
                canCheck = connected?.checks == true,
                checking = local.checking,
                checkError = local.checkError,
                canResetSerial = connected?.serialReset == true && job?.needsPhone != true,
                serialReset = local.serialReset,
                channels = connected?.channels == true,
                agentVersion = connected?.info?.agentVersion ?: unit?.profile?.agentVersion,
            )
        }

    /** What the Debug card is made of besides the page: the catalogue, the pre-release switches and the menu's entry. */
    private val debugSide = combine(catalogue, switchedOn, sshEntry, ::DebugSide)

    private data class DebugSide(val catalogue: CatalogueState, val switchedOn: Set<String>, val entry: SshEntry)

    val uiState: StateFlow<HeadUnitDetailUiState> =
        combine(page, fixes.state, stored, debugSide) { page, fix, stored, side ->
            val offered = sshOffered(side.catalogue, page.installed, page.agentVersion, side.switchedOn)
            page.copy(
                fix = fix,
                laterFormat = laterFormat(page.installed, stored),
                debug = debugCard(page, offered, side.entry, headUnitId),
            )
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), HeadUnitDetailUiState())

    init {
        // Bluetooth switched off or on while the page is open changes what it says at once.
        viewModelScope.launch { bluetooth.on.collect { readBluetooth() } }
        refreshSsh(headUnitId)
    }

    /**
     * Reads what the phone's Bluetooth says of the head unit's serial endpoint: when the screen comes up, on
     * return to it, and after the permission prompt. Pairing and switching Bluetooth on happen outside the app;
     * Bluetooth being switched on or off is followed meanwhile ([PhoneBluetooth.on]).
     */
    fun refreshBluetooth() {
        // Also has [PhoneBluetooth.on] read again, which a change of permission does not announce.
        bluetooth.refreshConnected()
        viewModelScope.launch { readBluetooth() }
    }

    private suspend fun readBluetooth() {
        val status = bluetooth.serialStatus(repository.headUnit(headUnitId).first())
        local.update { it.copy(serial = status) }
    }

    /** Connect, after the question where a terminal or the SSH port of another head unit would end ([ConnectGuard]). */
    fun connect() {
        viewModelScope.launch { if (guard.mayConnect(headUnitId)) jobs.connect(headUnitId, byUser = true) }
    }

    /** One connect with the phone dialling the car as well, see [JobRepository.connectFromPhone]: after the same question. */
    fun connectFromPhone() {
        viewModelScope.launch { if (guard.mayConnect(headUnitId)) jobs.connectFromPhone(headUnitId) }
    }

    /** What "Copy diagnostics" copies, where the connect failed: see [SerialDiagnostics.report]. */
    fun diagnostics(): String = diagnostics.report(uiState.value.headUnit?.name, jobs.agentSerial(headUnitId))

    fun disconnect() {
        viewModelScope.launch { jobs.disconnect() }
    }

    /**
     * Has the agent started with the car from now on, or no longer. Switching it on where it is on already is
     * not idle: the agent then forgets the starts on record that did not last, and the car tries again.
     */
    fun setAutostart(on: Boolean) {
        if (local.value.autostartBusy) return
        // The set-up's card says how the set-up left things, which the switch now changes by hand.
        setup.dismiss(headUnitId)
        viewModelScope.launch {
            local.update { it.copy(autostartBusy = true, autostartError = null) }
            val failure = jobs.setAutostart(on).exceptionOrNull()
            local.update { it.copy(autostartBusy = false, autostartError = failure?.let { why -> why.message ?: "The head unit didn't answer" }) }
        }
    }

    /** Runs the set-up for starting with the car again from the top, after it stopped short. */
    fun retrySetup() = setup.retry()

    /** Asks the agent to restart iDrive, once the set-up is done: see [AutostartSetup.restart]. */
    fun restartForSetup() = setup.restart()

    /** Hides the set-up's card, once it has ended. */
    fun dismissSetup() = setup.dismiss(headUnitId)

    /** Runs the agent's checks again, by hand: what they find takes the place of what was kept, put aside or not. */
    fun recheck() {
        if (local.value.checking) return
        viewModelScope.launch {
            local.update { it.copy(checking = true, checkError = null) }
            val failure = jobs.recheck().exceptionOrNull()
            local.update { it.copy(checking = false, checkError = failure?.let { why -> why.message ?: "The head unit didn't answer" }) }
        }
    }

    /** Opens the confirmation of the fix of the check [id], which reads the check again first. */
    fun openFix(id: String) {
        val problem = uiState.value.checks?.problem(id) ?: return
        fixes.open(id, problem.title)
    }

    /**
     * The fix's button on the set-up's card, where it asks about a check: the same confirmation, whose yes goes to
     * the set-up.
     */
    fun openSetupRemoval() = fixes.openForSetup(setup, headUnitId)

    /** Skip on the set-up's card: see [AutostartSetup.skipRemoval]. */
    fun skipSetupRemoval() = setup.skipRemoval()

    fun confirmFix() = fixes.confirm()

    fun retryFix() = fixes.retry()

    fun closeFix() = fixes.close()

    /** Puts the problem of the check [id] aside: it is no longer shown here, until the checks are run again by hand. */
    fun dismissCheck(id: String) {
        viewModelScope.launch { jobs.dismissCheck(headUnitId, id) }
    }

    /**
     * Has the car end this phone's Bluetooth link, and the app connect again, once the user has confirmed it. It is
     * the app's, so it goes on when the page is left; what it came to is shown here in one line.
     */
    fun resetSerial() {
        if (local.value.serialReset == SerialResetUi.Working || !uiState.value.canResetSerial) return
        local.update { it.copy(serialReset = SerialResetUi.Working) }
        appScope.launch {
            val result = jobs.resetSerial()
            local.update { it.copy(serialReset = serialResetText(result)) }
        }
    }

    /** Restart iDrive on the agent's card: asks first, in the same question as the side menu's. */
    fun askRestart() = restartQuestion.ask(headUnitId)

    /** Reboot on the agent's card, where the agent says the head unit has to: at once, and a refusal goes under the card. */
    fun reboot() {
        viewModelScope.launch {
            local.update { it.copy(restartError = null) }
            jobs.restart(headUnitId, "reboot").onFailure { failure -> local.update { it.copy(restartError = failure.message) } }
            jobs.refresh()
        }
    }
}

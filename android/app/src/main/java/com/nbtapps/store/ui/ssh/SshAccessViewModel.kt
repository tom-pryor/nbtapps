// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.ssh

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.ssh.ACCESS_LOCAL_NETWORK
import com.nbtapps.store.data.ssh.AddClientResult
import com.nbtapps.store.data.ssh.AuthorizedClients
import com.nbtapps.store.data.ssh.ClientKey
import com.nbtapps.store.data.ssh.PortAddress
import com.nbtapps.store.data.ssh.PortOffReason
import com.nbtapps.store.data.ssh.SshPort
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.ssh.SshShell
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.ssh.localNetworkNeeded
import com.nbtapps.store.model.ChannelSwitch
import com.nbtapps.store.service.connectionsText
import com.nbtapps.store.ui.navigation.SshAccessRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** Where the head unit stands for the port, as the screen knows it. */
enum class SshReach {
    /** A link to it, SSH access on. */
    READY,

    /** No link to it. */
    NO_LINK,

    /** A link, and SSH access off, or not known yet. */
    SSH_OFF,

    /** A link, a job running on the head unit, and SSH access not known: the agent says nothing of it meanwhile. */
    JOB,
}

/** Why "Allow other devices" stays off: the Nearby devices permission was refused, and whether Android asks again. */
enum class LocalNetworkRefusal {
    /** Refused; switching on again asks again. */
    ASK_AGAIN,

    /** Refused, and Android asks no more: only the app's permissions in the phone's settings can allow it. */
    FOR_GOOD,
}

/** What the screen says under "Allow other devices" after a refusal of the Nearby devices permission. */
internal fun localNetworkRefusalText(refusal: LocalNetworkRefusal): String = when (refusal) {
    LocalNetworkRefusal.ASK_AGAIN -> LOCAL_NETWORK_ASK_AGAIN
    LocalNetworkRefusal.FOR_GOOD -> LOCAL_NETWORK_FOR_GOOD
}

/** What "Allow other devices" thrown to [allow] does. */
enum class OtherDevicesStep {
    /** Nothing: the port is not on. */
    NOTHING,

    /** The Nearby devices permission is asked for first. */
    ASK,

    /** Other devices are allowed, or no longer, at once. */
    SET,
}

/**
 * What "Allow other devices" thrown to [allow] does, where it may be set ([enabled]: the port is on) and Android still
 * wants the permission ([needed]): asked for before other devices are allowed, never before they are no longer.
 */
internal fun otherDevicesStep(allow: Boolean, enabled: Boolean, needed: Boolean): OtherDevicesStep = when {
    !enabled -> OtherDevicesStep.NOTHING
    allow && needed -> OtherDevicesStep.ASK
    else -> OtherDevicesStep.SET
}

/** Whether a refusal of the permission is for good: Android says it will not ask again only right after one. */
internal fun localNetworkRefusal(granted: Boolean, askAgain: Boolean): LocalNetworkRefusal? = when {
    granted -> null
    askAgain -> LocalNetworkRefusal.ASK_AGAIN
    else -> LocalNetworkRefusal.FOR_GOOD
}

internal const val LOCAL_NETWORK_ASK_AGAIN =
    "Other devices need the Nearby devices permission, which lets this app take connections from the phone's hotspot or " +
        "Wi-Fi. Switch Allow other devices on again to be asked once more."
internal const val LOCAL_NETWORK_FOR_GOOD =
    "Other devices need the Nearby devices permission, and Android won't ask for it again. Allow Nearby devices in this " +
        "app's permissions in the phone's settings, then switch Allow other devices on."

/** The add-key dialog: what is typed, and why the last Add was refused, if it was. */
data class AddKeyForm(val name: String = "", val line: String = "", val keyError: String? = null, val nameError: String? = null)

/**
 * One way to reach the port: its label (null where there is one way only), the command to copy, and what its copy
 * button is called.
 */
data class PortCommand(val label: String?, val command: String, val copyDescription: String = "Copy command")

data class SshAccessUiState(
    /** The head unit's name, under the title. */
    val name: String? = null,
    val reach: SshReach = SshReach.NO_LINK,
    val keys: List<ClientKey> = emptyList(),
    /** The port as it stands, whichever head unit it is for. */
    val port: SshPortState = SshPortState.Off(),
    /** This screen's head unit, by this app's id. */
    val headUnitId: String = "",
    /** The add-key dialog, where it is open. */
    val adding: AddKeyForm? = null,
    /** The key whose removal is being asked about. */
    val removing: ClientKey? = null,
    /** Why the port could not be switched on from here, where it was refused before it began. */
    val refusal: String? = null,
    /** The Nearby devices permission is to be asked for, before other devices are allowed: the screen asks. */
    val askLocalNetwork: Boolean = false,
    /** The permission was refused the last time it was asked for. */
    val localNetworkRefusal: LocalNetworkRefusal? = null,
) {
    /** The line under "Allow other devices" after a refusal of the permission. */
    val localNetworkLine: String? get() = localNetworkRefusal?.let(::localNetworkRefusalText)

    /** The port as it stands for this head unit: off where it is another's. */
    val own: SshPortState get() = port.takeIf { it.headUnitId == headUnitId } ?: SshPortState.Off(headUnitId)

    /** The port is on, or being switched on, for another head unit. */
    val elsewhere: Boolean get() = port.active && port.headUnitId != headUnitId

    val on: SshPortState.On? get() = own as? SshPortState.On

    /** The switch "Let other apps connect" shows on: the port is on, or being switched on, here. */
    val switchOn: Boolean get() = own.active

    /** The switch may be thrown: off with all it needs, or on here; nothing while it checks. */
    val switchEnabled: Boolean get() = when (own) {
        is SshPortState.On -> true
        is SshPortState.Checking -> false
        is SshPortState.Off -> keys.isNotEmpty() && reach == SshReach.READY && !elsewhere
    }

    /** The line under the switch's name. */
    val switchLine: String get() = when (val now = own) {
        is SshPortState.Checking -> CHECKING
        is SshPortState.On -> "On · port ${now.port} · ${connectionsText(now.connections)}"
        is SshPortState.Off -> when {
            elsewhere -> "On for another head unit"
            keys.isEmpty() -> "Add a key first"
            reach == SshReach.NO_LINK -> "Connect to the head unit first"
            reach == SshReach.JOB -> "A job runs on the head unit. The port can be switched on once it has finished."
            reach == SshReach.SSH_OFF -> "Switch on SSH access first"
            else -> "Off"
        }
    }

    /** The line under the switch's row: why it went off by itself, in the ordinary colour. */
    val offReason: String? get() = (own as? SshPortState.Off)?.reason?.let(::offReasonText)

    /** The line under the switch's row in the error colour: why it could not be switched on. */
    val failure: String? get() = refusal ?: (own as? SshPortState.Off)?.failure

    /** "Allow other devices" may be set: only while the port is on. */
    val otherDevicesEnabled: Boolean get() = on != null

    /** What "Allow other devices" says under its name. */
    val otherDevicesLine: String get() = if (otherDevicesEnabled) OTHER_DEVICES else "$OTHER_DEVICES. Can be set while the port is on."

    /** A job runs on the head unit, which holds the port's connections. */
    val paused: Boolean get() = on?.paused == true

    /** The commands that reach the port, each with its address's label where there is more than one. */
    val commands: List<PortCommand> get() = on?.let(::portCommands).orEmpty()

    /** Add key may be pressed: fewer than the most keys are saved. */
    val canAdd: Boolean get() = keys.size < AuthorizedClients.MAX_CLIENTS
}

internal const val CHECKING = "Checking the way to the head unit…"
internal const val OTHER_DEVICES = "Lets a laptop on this phone's hotspot or Wi-Fi connect"
internal const val OTHER_DEVICES_WARNING = "Anyone on this phone's hotspot or Wi-Fi can reach the port. Only the keys below let them in."
internal const val IDLE_LINE = "Turns itself off after 30 minutes with no connection."
internal const val NO_KEYS_TEXT = "No keys yet. Paste the public key of the SSH client you want to let in."
internal const val KEYS_FULL = "7 keys at most. Remove one to add another."
internal const val BAD_LINE = "That isn't a public key line. Paste the whole line, starting with ssh-ed25519 or ecdsa-sha2-nistp256."
internal const val BAD_NAME = "Give it a name of 1 to 64 characters."
internal const val ELSEWHERE_REFUSED = "The port is on for another head unit. Turn it off there first."
internal const val HOST_KEY_TEXT = "Your SSH client shows this the first time it connects. Check that it is the same."

/** What the screen says of a port that went off by itself. */
internal fun offReasonText(reason: PortOffReason): String = when (reason) {
    PortOffReason.IDLE -> "Turned off after 30 minutes with no connection"
    PortOffReason.LINK_LOST -> "Turned off: the link to the head unit was lost"
    PortOffReason.SWITCHED_OFF -> "Turned off: SSH access was switched off"
    PortOffReason.PACKAGE_CHANGED -> "Turned off: the SSH package changed"
}

/** The label of an address's kind, as the screen shows it over its command. */
internal fun addressLabel(kind: PortAddress.Kind): String = when (kind) {
    PortAddress.Kind.THIS_PHONE -> "This phone"
    PortAddress.Kind.HOTSPOT -> "Hotspot"
    PortAddress.Kind.WIFI -> "Wi-Fi"
    PortAddress.Kind.NETWORK -> "Network"
}

/** The command for [port] at [host]: `ssh -p PORT root@HOST`, root being the user on a head unit. */
internal fun sshCommand(port: Int, host: String): String = "ssh -p $port ${SshShell.ROOT}@$host"

/** The commands for the port as [on] has it: one, with no label, for this phone alone; one an address otherwise. */
internal fun portCommands(on: SshPortState.On): List<PortCommand> {
    val many = on.addresses.size > 1
    return on.addresses.map { address ->
        if (!many) {
            PortCommand(null, sshCommand(on.port, address.host))
        } else {
            val which = when (address.kind) {
                PortAddress.Kind.THIS_PHONE -> "this phone"
                PortAddress.Kind.HOTSPOT -> "the hotspot"
                PortAddress.Kind.WIFI -> "Wi-Fi"
                PortAddress.Kind.NETWORK -> "the network"
            }
            PortCommand(addressLabel(address.kind), sshCommand(on.port, address.host), "Copy command for $which")
        }
    }
}

/** A fingerprint shortened for a key's row: `SHA256:` and its first eight and last four characters. */
internal fun shortFingerprint(fingerprint: String): String {
    val body = fingerprint.removePrefix(SHA256)
    return if (body.length <= 12) fingerprint else "$SHA256${body.take(8)}…${body.takeLast(4)}"
}

private const val SHA256 = "SHA256:"

/** What the add-key dialog says of a refusal of [AuthorizedClients.add]; null where the key was added. */
internal fun addRefusal(result: AddClientResult, form: AddKeyForm): AddKeyForm? = when (result) {
    is AddClientResult.Added -> null
    is AddClientResult.BadLine -> form.copy(keyError = BAD_LINE, nameError = null)
    AddClientResult.BadName -> form.copy(nameError = BAD_NAME, keyError = null)
    is AddClientResult.Already -> form.copy(keyError = "That key is saved already, as ${result.name}.", nameError = null)
    AddClientResult.Full -> form.copy(keyError = KEYS_FULL, nameError = null)
}

/**
 * The screen "SSH from other apps" of one head unit ([SshAccessRoute]): the port ([SshPort]) and the keys of the other
 * SSH clients ([AuthorizedClients]). It reads whether SSH access is on when the link to the head unit comes up and
 * when the port's state changes.
 */
@HiltViewModel
class SshAccessViewModel internal constructor(
    private val headUnitId: String,
    private val port: SshPort,
    private val clients: AuthorizedClients,
    private val connection: StateFlow<ConnectionState>,
    name: Flow<String?>,
    /** Reads SSH access on the head unit: [JobRepository.channelSwitch]. */
    private val readSwitch: suspend () -> Result<ChannelSwitch>,
    /** Whether a job runs on the head unit now, which the agent answers nothing of SSH access during. */
    private val jobRuns: Flow<Boolean> = flowOf(false),
    /** Whether the Nearby devices permission is still to be asked for before other devices are let in: [localNetworkNeeded]. */
    private val localNetworkNeeded: () -> Boolean = { false },
) : ViewModel() {

    @Inject
    constructor(
        savedStateHandle: SavedStateHandle,
        port: SshPort,
        clients: AuthorizedClients,
        jobs: JobRepository,
        headUnits: HeadUnitRepository,
        @ApplicationContext context: Context,
    ) : this(
        savedStateHandle.toRoute<SshAccessRoute>().headUnitId,
        port,
        clients,
        jobs.connection,
        headUnits.headUnit(savedStateHandle.toRoute<SshAccessRoute>().headUnitId).map { it?.name },
        { jobs.channelSwitch(savedStateHandle.toRoute<SshAccessRoute>().headUnitId, SshTunnel.PACKAGE, SshTunnel.CHANNEL) },
        jobs.job.map { view ->
            view != null && view.headUnitId == savedStateHandle.toRoute<SshAccessRoute>().headUnitId && !view.ended && !view.linkLost && !view.forgotten
        },
        {
            localNetworkNeeded(
                Build.VERSION.SDK_INT,
                context.applicationInfo.targetSdkVersion,
                context.checkSelfPermission(ACCESS_LOCAL_NETWORK) == PackageManager.PERMISSION_GRANTED,
            )
        },
    )

    private data class Local(
        val sshOn: Boolean? = null,
        val adding: AddKeyForm? = null,
        val removing: ClientKey? = null,
        val refusal: String? = null,
        val askLocalNetwork: Boolean = false,
        val localNetworkRefusal: LocalNetworkRefusal? = null,
    )

    private val local = MutableStateFlow(Local())

    /** The link to this head unit, if it is up, and whether a job runs on it. */
    private data class Linked(val connected: ConnectionState.Connected?, val jobRuns: Boolean)

    private val linked = combine(connection, jobRuns) { now, job ->
        Linked((now as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }, job)
    }

    val uiState: StateFlow<SshAccessUiState> =
        combine(name, linked, clients.clients, port.state, local) { name, linked, keys, port, local ->
            SshAccessUiState(
                name = name,
                reach = when {
                    linked.connected == null -> SshReach.NO_LINK
                    local.sshOn == true -> SshReach.READY
                    // Not known while a job runs: nothing is said of a switch that may well be on.
                    linked.jobRuns && local.sshOn == null -> SshReach.JOB
                    else -> SshReach.SSH_OFF
                },
                keys = keys,
                port = port,
                headUnitId = headUnitId,
                adding = local.adding,
                removing = local.removing,
                refusal = local.refusal,
                askLocalNetwork = local.askLocalNetwork,
                localNetworkRefusal = local.localNetworkRefusal,
            )
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), SshAccessUiState(headUnitId = headUnitId))

    init {
        viewModelScope.launch { followSwitch() }
    }

    /**
     * Reads SSH access whenever a link to the head unit comes up with the package, when a job on it begins or ends, and
     * when the port's state changes.
     */
    private suspend fun followSwitch() {
        combine(linked, port.state) { linked, port ->
            linked.connected?.installed?.find { it.id == SshTunnel.PACKAGE }?.let { pkg -> Triple(pkg.version, linked.jobRuns, port) }
        }.distinctUntilChanged().collectLatest { look ->
            if (look == null) {
                local.update { it.copy(sshOn = null) }
            } else {
                readSwitch().onSuccess { answer -> local.update { it.copy(sshOn = answer.on) } }
            }
        }
    }

    /** The switch "Let other apps connect" thrown. */
    fun setPort(on: Boolean) {
        local.update { it.copy(refusal = null) }
        if (!on) {
            if (uiState.value.own.active) port.switchOff()
            return
        }
        if (!uiState.value.switchEnabled) return
        if (!port.switchOn(headUnitId)) local.update { it.copy(refusal = ELSEWHERE_REFUSED) }
    }

    /**
     * "Allow other devices" thrown: at once, with no question of the app's own. Where Android wants the Nearby devices
     * permission for it, that is asked for first ([SshAccessUiState.askLocalNetwork]), and other devices are allowed
     * once it is granted ([localNetworkAnswered]).
     */
    fun setOtherDevices(allow: Boolean) {
        when (otherDevicesStep(allow, uiState.value.otherDevicesEnabled, localNetworkNeeded())) {
            OtherDevicesStep.NOTHING -> return
            OtherDevicesStep.ASK -> local.update { it.copy(localNetworkRefusal = null, askLocalNetwork = true) }
            OtherDevicesStep.SET -> {
                local.update { it.copy(localNetworkRefusal = null) }
                port.allowOtherDevices(allow)
            }
        }
    }

    /** The screen has put Android's question up, or found it would not: it is not put up again. */
    fun localNetworkAsked() = local.update { it.copy(askLocalNetwork = false) }

    /**
     * Android's answer to the Nearby devices permission: [granted], and where it was refused, whether Android would
     * [askAgain]. Granted, other devices are allowed, as the switch was thrown; refused, the switch stays off with why.
     * A phone where Bluetooth is allowed already answers at once, with no question shown.
     */
    fun localNetworkAnswered(granted: Boolean, askAgain: Boolean) {
        val refusal = localNetworkRefusal(granted, askAgain)
        local.update { it.copy(askLocalNetwork = false, localNetworkRefusal = refusal) }
        if (refusal == null && uiState.value.otherDevicesEnabled) port.allowOtherDevices(true)
    }

    fun openAdd() {
        if (uiState.value.canAdd) local.update { it.copy(adding = AddKeyForm()) }
    }

    fun editAdd(form: AddKeyForm) = local.update { it.copy(adding = form.copy(keyError = null, nameError = null)) }

    fun cancelAdd() = local.update { it.copy(adding = null) }

    /** Add in the dialog: the key is saved, or the dialog stays open with why it was not. */
    fun confirmAdd() {
        val form = local.value.adding ?: return
        viewModelScope.launch {
            val refused = addRefusal(clients.add(form.name, form.line), form)
            local.update { it.copy(adding = refused) }
        }
    }

    fun askRemove(key: ClientKey) = local.update { it.copy(removing = key) }

    fun cancelRemove() = local.update { it.copy(removing = null) }

    /** Remove in the question: the key goes, and the port closes its connections ([SshPort]). */
    fun confirmRemove() {
        val key = local.value.removing ?: return
        local.update { it.copy(removing = null) }
        viewModelScope.launch { clients.remove(key.fingerprint) }
    }
}

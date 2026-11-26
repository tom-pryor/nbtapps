// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PrereleaseRepository
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.packageStatus
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.di.ApplicationScope
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.ChannelError
import com.nbtapps.store.model.ChannelSwitch
import com.nbtapps.store.model.InstalledPackage
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * What the app knows of the SSH package's switch on the head unit it is connected to, and how a throw of it from the
 * app goes: [SshAccess.state].
 */
data class SshSwitchState(
    /**
     * The head unit it is of: the one the app is connected to, whose agent has channels and which has the SSH package.
     * Null where there is none, and then nothing else is known either.
     */
    val headUnitId: String? = null,
    /** Whether SSH access is switched on, as the agent said last over the link. Null where it has not said yet. */
    val on: Boolean? = null,
    /**
     * How many SSH connections are open on the head unit, on every link, from the same answer. It is read again when
     * the link comes up, after each throw of the switch, and when a job, the terminal or the port changes; it is not
     * followed in between. Null where the agent's answer gave none.
     */
    val connections: Int? = null,
    /** The switch has been thrown, and the agent has not answered yet. */
    val switching: Boolean = false,
    /**
     * Why the agent did not take the last throw of the switch, in a line for the user: until the next throw, or the next
     * time the switch is read when the link, a job, the terminal or the port changes.
     */
    val error: String? = null,
    /** The question before SSH access is switched on is open. */
    val confirming: Boolean = false,
) {
    /** The switch may be thrown: how it stands is known, and nothing waits for the agent. */
    val canSwitch: Boolean get() = on != null && !switching
}

/**
 * The SSH package's switch on the head unit the app is connected to, for every screen that shows it: the side menu,
 * the SSH page and the head unit's page. It lives as long as the process.
 *
 * It follows the app's link: the switch is read (`channel.get`) when a link comes up to a head unit whose agent has
 * channels and which has the SSH package, once per such link whether a page that shows it is open or not, and again
 * when the package's version changes, when a job begins or ends on that head unit, when the terminal's or the SSH
 * port's state on it changes, and when a page that shows it opens ([refresh]). Nothing is read from a head unit
 * without the package. With no such link what the agent said is forgotten.
 *
 * Switching on asks first ([switch], then [confirm] or [cancel]); switching off is done at once, and tells the SSH
 * port, which goes off with it.
 */
@Singleton
class SshAccess internal constructor(
    /** The app's link: [JobRepository.connection]. */
    private val connection: StateFlow<ConnectionState>,
    /** The head unit a job runs on, or null where none runs. */
    jobOn: Flow<String?>,
    /** The app's terminal: [TerminalRepository.state]. */
    terminal: Flow<TerminalState>,
    /** The SSH port for other apps: [SshPort.state]. */
    port: Flow<SshPortState>,
    /** Reads the switch on a head unit: [JobRepository.channelSwitch] for the SSH package's channel. */
    private val read: suspend (headUnitId: String) -> Result<ChannelSwitch>,
    /** Throws the switch on a head unit: [JobRepository.setChannelSwitch] for the SSH package's channel. */
    private val write: suspend (headUnitId: String, on: Boolean) -> Result<ChannelSwitch>,
    /** Told when SSH access was switched on or off from the app: [SshPort.noteSwitch]. */
    private val noteSwitch: (headUnitId: String, on: Boolean) -> Unit,
    private val scope: CoroutineScope,
    /** The catalogue, for whether it offers the SSH package: see [entry]. */
    catalogue: Flow<CatalogueState> = flowOf(CatalogueState()),
    /** The packages whose pre-releases are offered. */
    switchedOn: Flow<Set<String>> = flowOf(emptySet()),
    /** The saved head units' names, by id. */
    names: Flow<Map<String, String>> = flowOf(emptyMap()),
) {

    @Inject
    constructor(
        jobs: JobRepository,
        terminal: TerminalRepository,
        port: SshPort,
        catalogue: CatalogueRepository,
        prereleases: PrereleaseRepository,
        headUnits: HeadUnitRepository,
        @ApplicationScope scope: CoroutineScope,
    ) : this(
        jobs.connection,
        jobs.job.map { view -> view?.takeIf { !it.ended }?.headUnitId },
        terminal.state,
        port.state,
        { id -> jobs.channelSwitch(id, SshTunnel.PACKAGE, SshTunnel.CHANNEL) },
        { id, on -> jobs.setChannelSwitch(id, SshTunnel.PACKAGE, SshTunnel.CHANNEL, on) },
        port::noteSwitch,
        scope,
        catalogue.catalogue,
        prereleases.switchedOn,
        headUnits.headUnits.map { units -> units.associate { it.id to it.name } },
    )

    private val _state = MutableStateFlow(SshSwitchState())

    /** The switch, as the agent last said over the link that is open, and the throw of it from the app. */
    val state: StateFlow<SshSwitchState> = _state.asStateFlow()

    /** One request about the switch at a time, so that an older answer never takes the place of a newer one. */
    private val lock = Mutex()

    /** Counts the pages' asks for a reading: see [refresh]. */
    private val asked = MutableStateFlow(0)

    /** What a reading of the switch follows. */
    private data class Look(
        val headUnitId: String,
        val version: String,
        val jobRunning: Boolean,
        val terminal: TerminalState?,
        val port: SshPortState?,
        val asked: Int,
    )

    init {
        scope.launch {
            combine(connection, jobOn, terminal, port, asked) { connection, jobOn, terminal, port, asked ->
                val connected = (connection as? ConnectionState.Connected)?.takeIf { it.channels }
                connected?.installed?.find { it.id == SshTunnel.PACKAGE }?.let { pkg ->
                    val id = connected.headUnitId
                    Look(id, pkg.version, jobOn == id, terminal.takeIf { it.headUnitId == id }, port.takeIf { it.headUnitId == id }, asked)
                }
            }.distinctUntilChanged().collectLatest { look ->
                if (look == null) {
                    _state.value = SshSwitchState()
                } else {
                    // What was said of another head unit is not this one's.
                    if (_state.value.headUnitId != look.headUnitId) _state.value = SshSwitchState(headUnitId = look.headUnitId)
                    readNow(look.headUnitId)
                }
            }
        }
    }

    /**
     * The menu's entry for SSH, as the link, the catalogue and the switch have it now: see [sshEntry]. It is kept up
     * for as long as the process lives.
     */
    val entry: StateFlow<SshEntry> =
        combine(
            combine(connection, names, catalogue, switchedOn, ::EntrySide),
            _state,
            terminal,
            port,
        ) { side, switch, terminal, port ->
            sshEntry(side.connection, side.names, side.catalogue, side.switchedOn, switch, terminal, port)
        }.stateIn(scope, SharingStarted.Eagerly, SshEntry.NotConnected)

    private data class EntrySide(
        val connection: ConnectionState,
        val names: Map<String, String>,
        val catalogue: CatalogueState,
        val switchedOn: Set<String>,
    )

    /**
     * Has the switch read again, as a page that shows it opens: only where the app is connected to [headUnitId], and
     * then as the link's own readings are, so not where it has no channels or no SSH package.
     */
    fun refresh(headUnitId: String) {
        if ((connection.value as? ConnectionState.Connected)?.headUnitId == headUnitId) asked.update { it + 1 }
    }

    /**
     * Reads the switch: an answer also puts away the line of a refusal, which was of a moment that has passed, such as
     * a job that has now ended. Where the agent does not answer, what it said before stands, the line too.
     */
    private suspend fun readNow(headUnitId: String) = lock.withLock {
        read(headUnitId).onSuccess { answer ->
            took(headUnitId, answer)
            updateFor(headUnitId) { it.copy(error = null) }
        }
    }

    /**
     * The switch thrown to [on] on the head unit [headUnitId]: on asks first ([confirm]), off is done at once. Nothing is
     * done where the switch is not of that head unit, its state is not known, or a throw waits for the agent.
     */
    fun switch(headUnitId: String, on: Boolean) {
        val now = _state.value
        if (now.headUnitId != headUnitId || !now.canSwitch) return
        if (on) _state.update { it.copy(confirming = true) } else apply(headUnitId, false)
    }

    /** The question before SSH access is switched on, on the head unit [headUnitId], is answered yes. */
    fun confirm(headUnitId: String) {
        val now = _state.value
        if (!now.confirming || now.headUnitId != headUnitId) return
        _state.update { it.copy(confirming = false) }
        apply(headUnitId, true)
    }

    /** The question before SSH access is switched on is answered no, or put away. */
    fun cancel() = _state.update { it.copy(confirming = false) }

    /**
     * Has the agent switch SSH access [on] or off. Its answer says how the switch then stands; a refusal is told in a
     * line, and the switch is read again so that it shows what the head unit has. That reading leaves the line, which
     * goes at the next throw or the next reading that follows the link.
     */
    private fun apply(headUnitId: String, on: Boolean) {
        _state.update { it.copy(switching = true, error = null) }
        scope.launch {
            lock.withLock {
                write(headUnitId, on)
                    .onSuccess { answer ->
                        took(headUnitId, answer)
                        // The port for other apps goes off at once with it.
                        noteSwitch(headUnitId, answer.on)
                    }
                    .onFailure { failure ->
                        updateFor(headUnitId) { it.copy(error = sshSwitchError(failure)) }
                        read(headUnitId).onSuccess { took(headUnitId, it) }
                    }
                updateFor(headUnitId) { it.copy(switching = false) }
            }
        }
    }

    private fun took(headUnitId: String, answer: ChannelSwitch) = updateFor(headUnitId) { it.copy(on = answer.on, connections = answer.open) }

    /** Changes the state where it is still of [headUnitId]: an answer that comes after the link has gone is dropped. */
    private fun updateFor(headUnitId: String, change: (SshSwitchState) -> SshSwitchState) =
        _state.update { if (it.headUnitId == headUnitId) change(it) else it }
}

/**
 * Whether the catalogue offers the SSH package for a head unit that has [installed] and runs agent [agentVersion]: a
 * version of it runs on that agent and this app may install it, as the package's page has it.
 */
fun sshOffered(catalogue: CatalogueState, installed: List<InstalledPackage>, agentVersion: String?, switchedOn: Set<String>): Boolean {
    val pkg = catalogue.index?.packages?.find { it.id == SshTunnel.PACKAGE } ?: return false
    return packageStatus(pkg, installed, agentVersion, switchedOn)?.suits == true
}

/** What the app says where the agent did not take a throw of the switch, or the link failed under it. */
fun sshSwitchError(failure: Throwable): String {
    val code = (failure as? AgentRefusal)?.code ?: return failure.message ?: "The head unit didn't answer"
    return when (code) {
        ChannelError.BUSY -> "The head unit is busy with a job. Try again once it has finished."
        ChannelError.NOT_FOUND -> "SSH access isn't installed on the head unit any more."
        ChannelError.UNSUPPORTED -> "This head unit's agent can't switch SSH access. Update the agent."
        IO_ERROR -> "The head unit couldn't change the switch."
        else -> failure.message ?: AgentText.request(code)
    }
}

/** The agent's code for a switch it could not change. */
private const val IO_ERROR = "io-error"

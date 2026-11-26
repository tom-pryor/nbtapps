// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.restart

import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.ssh.SshPort
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.AgentText
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/** Where the question before iDrive is restarted stands: see [RestartQuestion]. */
sealed interface RestartStep {
    /** No question is open. */
    data object Closed : RestartStep

    /**
     * The question is open, about the head unit [headUnitId]: [terminalOpen] where a terminal is open or being opened
     * on it, [portOpen] where the SSH port for other apps is on or being switched on for it, which both end with the link.
     */
    sealed interface Open : RestartStep {
        val headUnitId: String
        val terminalOpen: Boolean
        val portOpen: Boolean
    }

    /** It asks, with Cancel and Restart. */
    data class Asking(override val headUnitId: String, override val terminalOpen: Boolean, override val portOpen: Boolean) : Open

    /** The user said Restart, and the agent has not answered yet. */
    data class Waiting(override val headUnitId: String, override val terminalOpen: Boolean, override val portOpen: Boolean) : Open

    /**
     * The restart did not happen, or may not have: [text] says why, in red. Where the agent refused it, Restart asks
     * again; [linkEnded] where the link ended before the agent answered, and then there is only Close.
     */
    data class Failed(
        override val headUnitId: String,
        override val terminalOpen: Boolean,
        override val portOpen: Boolean,
        val text: String,
        val linkEnded: Boolean = false,
    ) : Open
}

/** What the dialog says where the link ended before the agent answered the restart. */
internal const val RESTART_LINK_ENDED = "The link ended before the head unit answered. iDrive may be restarting."

/**
 * The question before iDrive is restarted on a head unit, for one screen, which holds it in its view model with its
 * own [scope], so that the dialog stays over a rotation: the side menu, the head unit's page and the job's page each
 * have one. Nothing is asked of the agent before the user says Restart ([ask], then [confirm] or [cancel]).
 *
 * An answer of the agent's closes the question, and the link then ends by itself as iDrive restarts; the app does not
 * connect again by itself. A refusal is said in the dialog ([RestartStep.Failed]), where Restart tries again. While the
 * question asks or says a refusal, a link that ends or one to another head unit closes it. While it waits for the
 * agent's answer, nothing but the answer closes it: a link that ends then, with no answer, is said as such, since
 * iDrive may be restarting all the same.
 */
class RestartQuestion internal constructor(
    /** The app's link: [JobRepository.connection]. */
    private val connection: StateFlow<ConnectionState>,
    /** The app's terminal: [TerminalRepository.state]. */
    private val terminal: StateFlow<TerminalState>,
    /** The SSH port for other apps: [SshPort.state]. */
    private val port: StateFlow<SshPortState>,
    /** Restarts iDrive on a head unit: [JobRepository.restart], which fails unless the link is to that head unit. */
    private val restart: suspend (headUnitId: String) -> Result<Unit>,
    private val scope: CoroutineScope,
) {
    private val current = MutableStateFlow<RestartStep>(RestartStep.Closed)

    /** The dialog to show: none while [RestartStep.Closed]. */
    val state: StateFlow<RestartStep> = current.asStateFlow()

    private val lock = Any()

    init {
        scope.launch {
            combine(connection, terminal, port, ::Triple).collect { (connection, terminal, port) -> follow(connection, terminal, port) }
        }
    }

    /**
     * Asks whether to restart iDrive on the head unit [headUnitId], in place of a question that asks or says a refusal.
     * Nothing is asked where the link is not to that head unit, or while an answer is waited for.
     */
    fun ask(headUnitId: String) {
        synchronized(lock) {
            if (current.value is RestartStep.Waiting) return
            val now = connection.value
            if (connectedTo(now) != headUnitId) return
            current.value = RestartStep.Asking(headUnitId, terminalOpen(terminal.value, headUnitId), portOpen(port.value, headUnitId))
        }
    }

    /** Cancel, Close, Back or a tap beside the dialog: it closes, except while an answer is waited for. */
    fun cancel() {
        synchronized(lock) {
            if (current.value !is RestartStep.Waiting) current.value = RestartStep.Closed
        }
    }

    /**
     * Restart, while the question asks or says a refusal: the agent is asked, and the dialog waits for its answer. Where
     * the link is no longer to the head unit asked about, as when it ended a moment before the tap, nothing is sent and
     * the question closes.
     */
    fun confirm() {
        val waiting = synchronized(lock) {
            val now = current.value
            if (now !is RestartStep.Asking && !(now is RestartStep.Failed && !now.linkEnded)) return
            now as RestartStep.Open
            if (connectedTo(connection.value) != now.headUnitId) {
                current.value = RestartStep.Closed
                return
            }
            RestartStep.Waiting(now.headUnitId, now.terminalOpen, now.portOpen).also { current.value = it }
        }
        scope.launch {
            val failure = restart(waiting.headUnitId).exceptionOrNull()
            synchronized(lock) {
                if (current.value != waiting) return@synchronized
                current.value = when {
                    failure == null -> RestartStep.Closed
                    failure is AgentRefusal -> refused(waiting.headUnitId, AgentText.restartRefusal(failure.code))
                    else -> RestartStep.Failed(waiting.headUnitId, waiting.terminalOpen, waiting.portOpen, RESTART_LINK_ENDED, linkEnded = true)
                }
            }
        }
    }

    /**
     * The refusal [text] for the head unit [headUnitId], as things stand now: where the link went while the agent
     * answered, the question closes as it would have had it asked.
     */
    private fun refused(headUnitId: String, text: String): RestartStep {
        if (connectedTo(connection.value) != headUnitId) return RestartStep.Closed
        return RestartStep.Failed(headUnitId, terminalOpen(terminal.value, headUnitId), portOpen(port.value, headUnitId), text)
    }

    /** Keeps a question that asks or says a refusal up with the link, the terminal and the port. */
    private fun follow(connection: ConnectionState, terminal: TerminalState, port: SshPortState) {
        synchronized(lock) {
            val now = current.value
            val followed = now is RestartStep.Asking || (now is RestartStep.Failed && !now.linkEnded)
            if (!followed) return
            now as RestartStep.Open
            val id = now.headUnitId
            if (connectedTo(connection) != id) {
                current.value = RestartStep.Closed
                return
            }
            current.value = when (now) {
                is RestartStep.Asking -> now.copy(terminalOpen = terminalOpen(terminal, id), portOpen = portOpen(port, id))
                is RestartStep.Failed -> now.copy(terminalOpen = terminalOpen(terminal, id), portOpen = portOpen(port, id))
                else -> now
            }
        }
    }

    private fun connectedTo(connection: ConnectionState): String? = (connection as? ConnectionState.Connected)?.headUnitId

    private fun terminalOpen(terminal: TerminalState, headUnitId: String) = terminal.active && terminal.headUnitId == headUnitId

    private fun portOpen(port: SshPortState, headUnitId: String) = port.active && port.headUnitId == headUnitId
}

/** Makes each screen's [RestartQuestion], over the app's link, terminal and SSH port. */
class RestartQuestions internal constructor(
    private val connection: StateFlow<ConnectionState>,
    private val terminal: StateFlow<TerminalState>,
    private val port: StateFlow<SshPortState>,
    private val restart: suspend (headUnitId: String) -> Result<Unit>,
) {
    @Inject
    constructor(jobs: JobRepository, terminal: TerminalRepository, port: SshPort) :
        this(jobs.connection, terminal.state, port.state, { id -> jobs.restart(id, "hmi") })

    /** A question of its own for a screen whose view model's scope is [scope]. */
    fun question(scope: CoroutineScope) = RestartQuestion(connection, terminal, port, restart, scope)
}

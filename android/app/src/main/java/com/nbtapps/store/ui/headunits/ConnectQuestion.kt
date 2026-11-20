// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.ssh.SshPort
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.terminal.TerminalState
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.getAndUpdate
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withTimeoutOrNull

/** What the app asks before a connect of the user's that would end a terminal or the SSH port on another head unit. */
data class ConnectQuestion(val title: String, val text: String)

/**
 * The question before the user's connect to the head unit [targetId] takes the app's link from another head unit
 * with a terminal open or being opened ([terminal]) or the SSH port for other apps on or being switched on ([port]);
 * null where there is nothing to lose, as for a connect to the same head unit. [name] gives a head unit's name by its
 * id.
 */
fun connectQuestion(targetId: String, terminal: TerminalState, port: SshPortState, name: (String) -> String): ConnectQuestion? {
    val shell = terminal.headUnitId?.takeIf { terminal.active && it != targetId }
    val ssh = port.headUnitId?.takeIf { port.active && it != targetId }
    val target = name(targetId)
    val text = when {
        shell != null && ssh != null && shell == ssh ->
            "A terminal is open on ${name(shell)}, and SSH access for other apps is on for it. Connecting to $target " +
                "closes the terminal and turns SSH access for other apps off."
        shell != null && ssh != null ->
            "A terminal is open on ${name(shell)}, and SSH access for other apps is on for ${name(ssh)}. Connecting to " +
                "$target closes the terminal and turns SSH access for other apps off."
        shell != null -> "A terminal is open on ${name(shell)}. Connecting to $target closes it."
        ssh != null -> "SSH access for other apps is on for ${name(ssh)}. Connecting to $target turns it off."
        else -> return null
    }
    return ConnectQuestion("Connect to $target?", text)
}

/** What a connect the user starts goes through first: [ConnectGate] in the app. */
fun interface ConnectGuard {
    /** Whether the user's connect to the head unit [headUnitId] may go on: see [ConnectGate.mayConnect]. */
    suspend fun mayConnect(headUnitId: String): Boolean

    companion object {
        /** Lets every connect through: for a screen's model built without the app's, as in a test. */
        val NONE = ConnectGuard { true }
    }
}

/**
 * Stands before every connect the user starts, wherever it is started: one that would end a terminal or the SSH port
 * on another head unit ([connectQuestion]) waits for the user's answer to the question it puts up ([question], shown
 * by [ConnectQuestionDialog]). Links the app takes by itself do not come here.
 */
@Singleton
class ConnectGate internal constructor(
    /** How the terminal stands: [TerminalRepository.state]. */
    private val terminal: StateFlow<TerminalState>,
    /** The user's Close of the terminal: [TerminalRepository.close]. */
    private val closeTerminal: () -> Unit,
    /** How the SSH port for other apps stands: [SshPort.state]. */
    private val port: StateFlow<SshPortState>,
    /** The user's switch-off of the port: [SshPort.switchOff]. */
    private val switchOffPort: () -> Unit,
    /** The saved head units' names, by id. */
    private val names: Flow<Map<String, String>>,
) : ConnectGuard {

    @Inject
    constructor(terminal: TerminalRepository, port: SshPort, headUnits: HeadUnitRepository) : this(
        terminal.state,
        terminal::close,
        port.state,
        port::switchOff,
        headUnits.headUnits.map { units -> units.associate { it.id to it.name } },
    )

    private class Asking(val question: ConnectQuestion, val answer: CompletableDeferred<Boolean>)

    private val asking = MutableStateFlow<Asking?>(null)

    private val _question = MutableStateFlow<ConnectQuestion?>(null)

    /** The question being asked, if one is. */
    val question: StateFlow<ConnectQuestion?> = _question.asStateFlow()

    /**
     * Whether the user's connect to the head unit [headUnitId] may go on: at once where nothing would be lost; where
     * something would, once the user has said Connect, and then with the terminal closed and the port switched off as
     * the user's own Close and switch-off do. False where the user said Cancel, or another question took this one's
     * place: nothing has changed then. A caller that is cancelled meanwhile takes its question with it.
     */
    override suspend fun mayConnect(headUnitId: String): Boolean {
        val known = names.first()
        val question = connectQuestion(headUnitId, terminal.value, port.value) { known[it] ?: OTHER } ?: return true
        val mine = Asking(question, CompletableDeferred())
        asking.getAndUpdate { mine }?.answer?.complete(false)
        _question.value = question
        val connect = try {
            mine.answer.await()
        } finally {
            if (asking.compareAndSet(mine, null)) _question.value = null
        }
        if (!connect) return false
        val elsewhere = { state: TerminalState -> state.active && state.headUnitId != headUnitId }
        val portElsewhere = { state: SshPortState -> state.active && state.headUnitId != headUnitId }
        if (elsewhere(terminal.value)) closeTerminal()
        if (portElsewhere(port.value)) switchOffPort()
        // Their ends are told as the user's before the link goes; one that takes longer is cut by the connect.
        withTimeoutOrNull(CLOSE_WAIT_MS) {
            terminal.first { !elsewhere(it) }
            port.first { !portElsewhere(it) }
        }
        return true
    }

    /** The user's answer to the [question]: Connect, or Cancel. */
    fun answer(connect: Boolean) {
        asking.value?.answer?.complete(connect)
    }

    companion object {
        /** How long a closed terminal and a switched-off port are given to end before the connect goes on. */
        const val CLOSE_WAIT_MS = 5_000L

        /** A head unit whose name is not known. */
        const val OTHER = "the other head unit"
    }
}

/** The [ConnectGate]'s question, where one is asked: Cancel and Connect. */
@Composable
fun ConnectQuestionDialog(gate: ConnectGate) {
    val question by gate.question.collectAsStateWithLifecycle()
    val now = question ?: return
    AlertDialog(
        onDismissRequest = { gate.answer(false) },
        title = { Text(now.title) },
        text = { Text(now.text) },
        confirmButton = { TextButton(onClick = { gate.answer(true) }) { Text("Connect") } },
        dismissButton = { TextButton(onClick = { gate.answer(false) }) { Text("Cancel") } },
    )
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.restart

import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.ssh.SshFixtures
import com.nbtapps.store.data.ssh.SshFixtures.CAR
import com.nbtapps.store.data.ssh.SshFixtures.OTHER
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.AgentText
import java.io.IOException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The question before iDrive is restarted, over a stand-in link whose state the test sets and a restart whose answer
 * the test gives, in virtual time: each test puts the link's changes, the tap and the agent's answer in the order it
 * means, and races are tested in both orders. Also the dialog's line for what is open.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class RestartQuestionTest {

    private val connection = MutableStateFlow<ConnectionState>(SshFixtures.connected(CAR))
    private val terminal = MutableStateFlow<TerminalState>(TerminalState.Idle)
    private val port = MutableStateFlow<SshPortState>(SshPortState.Off())

    /** The head units a restart was sent for, in order. */
    private val sent = mutableListOf<String>()

    /** The answer the next restart waits for. */
    private var answer = CompletableDeferred<Result<Unit>>()

    private fun TestScope.question(scope: CoroutineScope = backgroundScope) = RestartQuestion(
        connection,
        terminal,
        port,
        { id ->
            sent += id
            answer.await()
        },
        scope,
    ).also { runCurrent() }

    private val asking = RestartStep.Asking(CAR, terminalOpen = false, portOpen = false)
    private val waiting = RestartStep.Waiting(CAR, terminalOpen = false, portOpen = false)

    private fun refusal(code: String) = Result.failure<Unit>(AgentRefusal(code, AgentText.request(code)))

    // ---- The agent's answer ----

    @Test
    fun `asked and confirmed, it waits for the agent, and its answer closes the question`() = runTest {
        val question = question()
        question.ask(CAR)
        assertEquals(asking, question.state.value)
        assertEquals("nothing is sent before Restart", emptyList<String>(), sent)

        question.confirm()
        assertEquals(waiting, question.state.value)
        runCurrent()
        assertEquals(listOf(CAR), sent)

        answer.complete(Result.success(Unit))
        runCurrent()
        assertEquals(RestartStep.Closed, question.state.value)
    }

    @Test
    fun `busy is said in the dialog in the restart's own words, and Restart asks again`() = runTest {
        val question = question()
        question.ask(CAR)
        question.confirm()
        runCurrent()
        answer.complete(refusal("busy"))
        runCurrent()
        val busy = "The head unit is busy with a job or a restart. Try again when it's done."
        assertEquals(RestartStep.Failed(CAR, terminalOpen = false, portOpen = false, busy), question.state.value)

        answer = CompletableDeferred()
        question.confirm()
        assertEquals(waiting, question.state.value)
        runCurrent()
        assertEquals(listOf(CAR, CAR), sent)
        answer.complete(Result.success(Unit))
        runCurrent()
        assertEquals(RestartStep.Closed, question.state.value)
    }

    @Test
    fun `an agent that can't restart is said with the app's words for its code, and Cancel closes it`() = runTest {
        val question = question()
        question.ask(CAR)
        question.confirm()
        runCurrent()
        answer.complete(refusal("unsupported"))
        runCurrent()
        assertEquals(RestartStep.Failed(CAR, terminalOpen = false, portOpen = false, AgentText.request("unsupported")), question.state.value)

        question.cancel()
        assertEquals(RestartStep.Closed, question.state.value)
    }

    // ---- The link, against the question ----

    @Test
    fun `nothing is asked where the link is not to the head unit`() = runTest {
        val question = question()
        question.ask(OTHER)
        assertEquals(RestartStep.Closed, question.state.value)
        connection.value = ConnectionState.Disconnected
        question.ask(CAR)
        assertEquals(RestartStep.Closed, question.state.value)
    }

    @Test
    fun `a link that ends while it asks closes the question, and a Restart after it sends nothing`() = runTest {
        val question = question()
        question.ask(CAR)
        connection.value = ConnectionState.Disconnected
        runCurrent()
        assertEquals(RestartStep.Closed, question.state.value)

        question.confirm()
        runCurrent()
        assertEquals(RestartStep.Closed, question.state.value)
        assertEquals(emptyList<String>(), sent)
    }

    @Test
    fun `a Restart tapped after the link ended, before the question has followed it, sends nothing and closes it`() = runTest {
        val question = question()
        question.ask(CAR)
        connection.value = ConnectionState.Disconnected
        // The question has not followed the link yet: the tap comes first.
        question.confirm()
        assertEquals(RestartStep.Closed, question.state.value)
        runCurrent()
        assertEquals(RestartStep.Closed, question.state.value)
        assertEquals(emptyList<String>(), sent)
    }

    @Test
    fun `another head unit connected while it asks closes the question`() = runTest {
        val question = question()
        question.ask(CAR)
        connection.value = SshFixtures.connected(OTHER)
        runCurrent()
        assertEquals(RestartStep.Closed, question.state.value)
        question.confirm()
        runCurrent()
        assertEquals(emptyList<String>(), sent)
    }

    @Test
    fun `another head unit connected while it says a refusal closes the question too`() = runTest {
        val question = question()
        question.ask(CAR)
        question.confirm()
        runCurrent()
        answer.complete(refusal("busy"))
        runCurrent()
        connection.value = SshFixtures.connected(OTHER)
        runCurrent()
        assertEquals(RestartStep.Closed, question.state.value)
    }

    @Test
    fun `a link that ends while it waits, with no answer, is said as such, with only Close`() = runTest {
        val question = question()
        question.ask(CAR)
        question.confirm()
        runCurrent()
        // The link is seen to end first, and the call fails after it.
        connection.value = ConnectionState.Disconnected
        runCurrent()
        assertEquals("the link's end alone does not close it while it waits", waiting, question.state.value)
        answer.complete(Result.failure(IOException("The link to the head unit was lost")))
        runCurrent()
        val ended = RestartStep.Failed(CAR, terminalOpen = false, portOpen = false, RESTART_LINK_ENDED, linkEnded = true)
        assertEquals(ended, question.state.value)

        question.confirm()
        runCurrent()
        assertEquals("no Restart once the link has ended", ended, question.state.value)
        assertEquals(listOf(CAR), sent)
        question.cancel()
        assertEquals(RestartStep.Closed, question.state.value)
    }

    @Test
    fun `a call that fails before the link is seen to end is said the same, and stays once it is seen`() = runTest {
        val question = question()
        question.ask(CAR)
        question.confirm()
        runCurrent()
        // The other order: the call fails first, and the link is seen to end after it.
        answer.complete(Result.failure(IOException("The link to the head unit was lost")))
        runCurrent()
        connection.value = ConnectionState.Disconnected
        runCurrent()
        assertEquals(RestartStep.Failed(CAR, terminalOpen = false, portOpen = false, RESTART_LINK_ENDED, linkEnded = true), question.state.value)
    }

    @Test
    fun `the agent's answer closes it whether the link is seen to end before it or after it`() = runTest {
        val question = question()
        question.ask(CAR)
        question.confirm()
        runCurrent()
        answer.complete(Result.success(Unit))
        runCurrent()
        connection.value = ConnectionState.Disconnected
        runCurrent()
        assertEquals("answer first", RestartStep.Closed, question.state.value)

        connection.value = SshFixtures.connected(CAR)
        answer = CompletableDeferred()
        question.ask(CAR)
        question.confirm()
        runCurrent()
        connection.value = ConnectionState.Disconnected
        runCurrent()
        answer.complete(Result.success(Unit))
        runCurrent()
        assertEquals("link's end first", RestartStep.Closed, question.state.value)
    }

    @Test
    fun `a refusal that comes once the link has gone closes the question, as the link's end would have`() = runTest {
        val question = question()
        question.ask(CAR)
        question.confirm()
        runCurrent()
        connection.value = ConnectionState.Disconnected
        runCurrent()
        answer.complete(refusal("busy"))
        runCurrent()
        assertEquals(RestartStep.Closed, question.state.value)
    }

    @Test
    fun `while it waits, Cancel, Back and another ask leave it as it is`() = runTest {
        val question = question()
        question.ask(CAR)
        question.confirm()
        question.cancel()
        question.ask(CAR)
        assertEquals(waiting, question.state.value)
        runCurrent()
        assertEquals("one request only", listOf(CAR), sent)
    }

    @Test
    fun `a second ask while it asks leaves one question`() = runTest {
        val question = question()
        question.ask(CAR)
        question.ask(CAR)
        assertEquals(asking, question.state.value)
    }

    // ---- What is open on the head unit ----

    @Test
    fun `it says the terminal and the port that are open on the head unit, and follows them while it asks`() = runTest {
        terminal.value = TerminalState.Open(CAR)
        port.value = SshPortState.On(CAR, 2222, hostKey = "SHA256:x")
        val question = question()
        question.ask(CAR)
        assertEquals(RestartStep.Asking(CAR, terminalOpen = true, portOpen = true), question.state.value)

        terminal.value = TerminalState.Ended(CAR, TerminalEnd.ServerClosed)
        runCurrent()
        assertEquals(RestartStep.Asking(CAR, terminalOpen = false, portOpen = true), question.state.value)
        port.value = SshPortState.Off(CAR)
        terminal.value = TerminalState.Connecting(CAR)
        runCurrent()
        assertEquals(RestartStep.Asking(CAR, terminalOpen = true, portOpen = false), question.state.value)
    }

    @Test
    fun `a terminal or port of another head unit is not this one's`() = runTest {
        terminal.value = TerminalState.Open(OTHER)
        port.value = SshPortState.Checking(OTHER)
        val question = question()
        question.ask(CAR)
        assertEquals(asking, question.state.value)
    }

    @Test
    fun `the dialog's line says what closes besides, for each of the terminal and the port`() {
        assertNull(restartOpenLine(terminalOpen = false, portOpen = false))
        assertEquals("The open terminal closes too.", restartOpenLine(terminalOpen = true, portOpen = false))
        assertEquals("The SSH port for other apps closes too.", restartOpenLine(terminalOpen = false, portOpen = true))
        assertEquals("The open terminal and the SSH port for other apps close too.", restartOpenLine(terminalOpen = true, portOpen = true))
    }
}

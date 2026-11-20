// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.terminal

import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.MemoryLink
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.ssh.ShellEnd
import com.nbtapps.store.data.ssh.SshShellException
import com.nbtapps.store.data.ssh.SshTunnelException
import com.nbtapps.store.data.ssh.TunnelFailure
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.TcpEndpoint
import java.io.IOException
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.job
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.currentTime
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The terminal's session holder on the test clock, over a stand-in for the link ([FakeLink]) and for the tunnel and
 * the shell ([FakeOpener], [FakeShell]). The holder against the real agent and SSH package is in
 * [com.nbtapps.store.data.ssh.SshRealAgentTest].
 */
@OptIn(ExperimentalCoroutinesApi::class)
class TerminalRepositoryTest {

    /** The session the stand-in link gives: none of the stand-ins speaks over it. */
    private val sessionScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val session = AgentSession(MemoryLink.pair().first, sessionScope)

    @After
    fun tearDown() {
        session.close("the test is over")
        sessionScope.cancel()
    }

    private fun connected(headUnitId: String, channels: Boolean = true) = ConnectionState.Connected(
        headUnitId,
        TcpEndpoint("192.0.2.1", 7144),
        AgentInfo("1.7.0", "000102030405060708090a0b0c0d0e0f", 1),
        emptyList(),
        capabilities = if (channels) listOf(ProtocolConstants.CAPABILITY_CHANNELS) else emptyList(),
    )

    /** The app's link as the terminal sees it: a connect that is made at once, or fails, and a count of its uses. */
    private inner class FakeLink : TerminalLink {
        override val connection = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
        val connects = CopyOnWriteArrayList<String>()

        @Volatile
        var fails: String? = null

        @Volatile
        var channels = true

        /** A connect waits for it, while it is set. */
        @Volatile
        var gate: CompletableDeferred<Unit>? = null

        /** The uses of the link now. */
        val inUse = AtomicInteger()

        /** What [endReason] answers, after how long: as the link's own reason where it is not set. */
        @Volatile
        var settles: Pair<String, Long>? = null

        /** The reasons [endReason] was asked about, and the uses of the link there were then. */
        val asked = CopyOnWriteArrayList<Pair<String, Int>>()

        override suspend fun endReason(session: AgentSession, reason: String): String {
            asked += reason to inUse.get()
            val (settled, afterMs) = settles ?: return reason
            delay(afterMs)
            return settled
        }

        override suspend fun connect(headUnitId: String): Result<Unit> {
            connects += headUnitId
            gate?.await()
            fails?.let {
                connection.value = ConnectionState.Failed(headUnitId, it)
                return Result.failure(IOException(it))
            }
            connection.value = connected(headUnitId, channels)
            return Result.success(Unit)
        }

        override suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T> {
            if ((connection.value as? ConnectionState.Connected)?.headUnitId != headUnitId) return Result.failure(IOException("Not connected to a head unit"))
            inUse.incrementAndGet()
            return try {
                Result.success(block(session))
            } catch (e: IOException) {
                Result.failure(e)
            } finally {
                inUse.decrementAndGet()
            }
        }
    }

    /** A shell the test drives: what it writes ([says]), how it ends ([end]), its channel's pause. */
    private class FakeShell(capacity: Int) : TerminalShell {
        override val channel = MutableStateFlow<ChannelState>(ChannelState.Open)
        override val ended = CompletableDeferred<ShellEnd>()
        val output = Channel<ByteArray>(capacity)
        val written = CopyOnWriteArrayList<String>()
        val sizes = CopyOnWriteArrayList<Pair<Int, Int>>()
        val closes = AtomicInteger()

        override suspend fun read(): ByteArray? = output.receiveCatching().getOrNull()

        override suspend fun write(bytes: ByteArray) {
            if (!ended.isCompleted) written += bytes.decodeToString()
        }

        override suspend fun resize(columns: Int, rows: Int) {
            if (!ended.isCompleted) sizes += columns to rows
        }

        override suspend fun close(): ShellEnd {
            closes.incrementAndGet()
            end(ShellEnd.ByApp)
            return ended.await()
        }

        fun says(text: String) {
            output.trySend(text.encodeToByteArray())
        }

        /** Ends it as [how]: what it wrote before is still read. */
        fun end(how: ShellEnd) {
            output.close()
            ended.complete(how)
        }
    }

    /** Opens [FakeShell]s, or fails as [failure] says. */
    private class FakeOpener : ShellOpener {
        val opened = CopyOnWriteArrayList<FakeShell>()
        val sizes = CopyOnWriteArrayList<Pair<Int, Int>>()

        @Volatile
        var failure: Exception? = null

        /** An open waits for it, while it is set. */
        @Volatile
        var gate: CompletableDeferred<Unit>? = null

        /** The pieces of output a shell holds before it holds the server back. */
        @Volatile
        var capacity = Channel.UNLIMITED

        val gaveUp = AtomicInteger()

        override suspend fun open(session: AgentSession, columns: Int, rows: Int, scope: CoroutineScope): TerminalShell {
            sizes += columns to rows
            gate?.let { gate ->
                try {
                    gate.await()
                } catch (e: CancellationException) {
                    gaveUp.incrementAndGet()
                    throw e
                }
            }
            failure?.let { throw it }
            return FakeShell(capacity).also { opened += it }
        }
    }

    private inner class Rig(val scope: TestScope) {
        val link = FakeLink()
        val opener = FakeOpener()
        val kept = AtomicInteger()
        val terminal = TerminalRepository(link, opener, { kept.incrementAndGet() }, scope.backgroundScope, clock = { scope.testScheduler.currentTime })
        val shell: FakeShell get() = opener.opened.last()

        /** Runs what is due, and lets the lines be given out. */
        fun settle() {
            scope.runCurrent()
            scope.advanceTimeBy(TerminalRepository.PUBLISH_EVERY_MS)
            scope.runCurrent()
        }

        val lines: List<TerminalLine> get() = terminal.lines.value.lines
        val texts: List<String> get() = lines.map { it.text }

        /** Opens a shell on "car", connected already, and settles. */
        fun opened(columns: Int = 80, rows: Int = 24): FakeShell {
            if (link.connection.value !is ConnectionState.Connected) link.connection.value = connected(CAR)
            terminal.open(CAR, columns, rows)
            settle()
            assertTrue("${terminal.state.value}", terminal.state.value is TerminalState.Open)
            return shell
        }

        /** The coroutines left in the repository's scope: its giving out of the lines, and nothing of a shell's. */
        fun running(): Int = scope.backgroundScope.coroutineContext.job.children.count { it.isActive }
    }

    private fun test(block: suspend TestScope.(Rig) -> Unit) = runTest { block(Rig(this)) }

    @Test
    fun `with no link it connects first, opens a shell of the size given, holds the link, and shows what the shell writes`() = test { rig ->
        rig.terminal.open(CAR, 100, 30)
        assertEquals(TerminalState.Connecting(CAR), rig.terminal.state.value)
        assertTrue(rig.terminal.state.value.active)
        assertEquals(1, rig.kept.get())
        rig.settle()

        assertEquals(listOf(CAR), rig.link.connects)
        assertEquals(listOf(100 to 30), rig.opener.sizes)
        assertEquals(TerminalState.Open(CAR), rig.terminal.state.value)
        assertEquals(1, rig.link.inUse.get())

        rig.shell.says("hello\r\n$ ")
        rig.settle()
        assertEquals(listOf("hello", "$ "), rig.texts)
        assertEquals("hello\n$ ", rig.terminal.text())
    }

    @Test
    fun `a link to the head unit there is is used as it is, and a link to another is replaced by a connect`() = test { rig ->
        rig.link.connection.value = connected(CAR)
        rig.terminal.open(CAR, 80, 24)
        rig.settle()
        assertEquals(emptyList<String>(), rig.link.connects)
        assertTrue(rig.terminal.state.value is TerminalState.Open)
        rig.terminal.close()
        rig.settle()

        rig.link.connection.value = connected("other")
        rig.terminal.open(CAR, 80, 24)
        rig.settle()
        assertEquals(listOf(CAR), rig.link.connects)
        assertTrue(rig.terminal.state.value is TerminalState.Open)
    }

    @Test
    fun `what has to be true first, and a connect that fails, are told as states, with no shell tried`() = test { rig ->
        rig.link.fails = "The head unit couldn't be reached"
        rig.terminal.open(CAR, 80, 24)
        rig.settle()
        assertEquals(TerminalState.Failed(CAR, "The head unit couldn't be reached"), rig.terminal.state.value)
        assertFalse(rig.terminal.state.value.active)

        rig.link.fails = null
        rig.link.channels = false
        rig.terminal.open(CAR, 80, 24)
        rig.settle()
        assertEquals(TerminalState.Unsupported(CAR), rig.terminal.state.value)
        assertEquals(emptyList<Pair<Int, Int>>(), rig.opener.sizes)
        assertEquals(0, rig.link.inUse.get())
        assertEquals(emptyList<TerminalLine>(), rig.lines)
    }

    @Test
    fun `every failure of the tunnel and of the shell's start is told as a state, and leaves the lines as they were`() = test { rig ->
        val busy = "The head unit is busy with a job. Open the terminal once it has finished."
        val table = listOf(
            SshTunnelException(TunnelFailure.Refused("off", "")) to TerminalState.Off(CAR),
            SshTunnelException(TunnelFailure.Refused("not-found", "")) to TerminalState.NoPackage(CAR),
            SshTunnelException(TunnelFailure.Refused("unsupported", "")) to TerminalState.Unsupported(CAR),
            SshTunnelException(TunnelFailure.Refused("busy", "")) to TerminalState.Failed(CAR, busy),
            SshTunnelException(TunnelFailure.Refused("unavailable", "")) to TerminalState.Failed(CAR, "The SSH package on the head unit didn't start"),
            SshTunnelException(TunnelFailure.Refused("too-many", "")) to
                TerminalState.Failed(CAR, "The link to the head unit has as many connections open as it can take"),
            SshTunnelException(TunnelFailure.Refused("forbidden", "")) to TerminalState.Failed(CAR, AgentText.request("forbidden")),
            SshTunnelException(TunnelFailure.Refused("a-later-code", "")) to TerminalState.Failed(CAR, AgentText.request("a-later-code")),
            SshTunnelException(TunnelFailure.Relay("too-many")) to TerminalState.Failed(CAR, "The head unit's SSH server has as many connections as it takes"),
            SshTunnelException(TunnelFailure.Relay("failed")) to TerminalState.Failed(CAR, "The head unit's SSH server couldn't be started"),
            SshTunnelException(TunnelFailure.Relay("bad-keys")) to TerminalState.Failed(CAR, "The SSH package on the head unit didn't take this phone's key"),
            SshTunnelException(TunnelFailure.Relay("later")) to TerminalState.Failed(CAR, "The SSH package on the head unit refused the connection (later)"),
            SshTunnelException(TunnelFailure.BadAnswer("no pty")) to TerminalState.Failed(CAR, "The SSH package on the head unit sent an answer this app can't read"),
            SshTunnelException(TunnelFailure.Closed(ChannelEnd.ByAgent("off"))) to TerminalState.Off(CAR),
            SshTunnelException(TunnelFailure.Closed(ChannelEnd.ByAgent("package-changed"))) to
                TerminalState.Failed(CAR, "The SSH package was being changed on the head unit. Try again once the job has finished."),
            SshTunnelException(TunnelFailure.Closed(ChannelEnd.ByAgent("closed"))) to TerminalState.Failed(CAR, "The head unit closed the SSH connection before it was up"),
            SshTunnelException(TunnelFailure.Closed(ChannelEnd.LinkLost("The head unit stopped answering"))) to TerminalState.Failed(CAR, "The head unit stopped answering"),
            SshTunnelException(TunnelFailure.Closed(ChannelEnd.AgentFault("an empty message"))) to
                TerminalState.Failed(CAR, "The head unit broke the channel's rules: an empty message"),
            SshTunnelException(TunnelFailure.NoAnswer(toKeys = false)) to TerminalState.Failed(CAR, "The head unit didn't open SSH in time"),
            SshTunnelException(TunnelFailure.NoAnswer(toKeys = true)) to TerminalState.Failed(CAR, "The SSH package on the head unit didn't answer in time"),
            SshShellException(ShellEnd.NoPty) to TerminalState.Failed(CAR, "The head unit has no pty driver running, so it can't give a terminal"),
            SshShellException(ShellEnd.HostKeyRefused) to
                TerminalState.Failed(CAR, "The head unit's SSH server isn't the one the agent named, so nothing was sent to it"),
            SshShellException(ShellEnd.LoginRefused) to TerminalState.Failed(CAR, "The head unit's SSH server didn't take this phone's key"),
            SshShellException(ShellEnd.Failed("The head unit's SSH server did not answer in time")) to
                TerminalState.Failed(CAR, "The head unit's SSH server did not answer in time"),
            SshShellException(ShellEnd.TunnelCut(ChannelEnd.ByAgent("off"))) to TerminalState.Off(CAR),
            SshShellException(ShellEnd.TunnelCut(ChannelEnd.LinkLost("Disconnected"))) to TerminalState.Failed(CAR, "Disconnected"),
            IOException("The phone's key store failed: gone") to TerminalState.Failed(CAR, "The phone's key store failed: gone"),
        )
        // Lines of a shell before, which stay.
        rig.opened().says("before\n")
        rig.terminal.close()
        rig.settle()
        val before = rig.lines

        for ((failure, expected) in table) {
            rig.opener.failure = failure
            rig.terminal.open(CAR, 80, 24)
            rig.settle()
            assertEquals("$failure", expected, rig.terminal.state.value)
            assertEquals(0, rig.link.inUse.get())
            assertEquals(before, rig.lines)
        }
        assertEquals(1, rig.opener.opened.size)
    }

    @Test
    fun `every end of an open shell is told as a state, below its lines, and the next shell opens below them`() = test { rig ->
        val table = listOf(
            ShellEnd.Exited(0) to TerminalState.Ended(CAR, TerminalEnd.Exited(0)),
            ShellEnd.Exited(130) to TerminalState.Ended(CAR, TerminalEnd.Exited(130)),
            ShellEnd.ServerClosed to TerminalState.Ended(CAR, TerminalEnd.ServerClosed),
            ShellEnd.TunnelCut(ChannelEnd.ByAgent("off")) to TerminalState.Ended(CAR, TerminalEnd.SwitchedOff),
            ShellEnd.TunnelCut(ChannelEnd.ByAgent("package-changed")) to TerminalState.Ended(CAR, TerminalEnd.PackageChanged),
            ShellEnd.TunnelCut(ChannelEnd.ByAgent("closed")) to TerminalState.Ended(CAR, TerminalEnd.ServerClosed),
            ShellEnd.TunnelCut(ChannelEnd.ByAgent("later")) to TerminalState.Failed(CAR, "The head unit closed the SSH connection (later)"),
            ShellEnd.TunnelCut(ChannelEnd.LinkLost("The head unit stopped answering")) to
                TerminalState.Ended(CAR, TerminalEnd.LinkLost("The head unit stopped answering")),
            ShellEnd.TunnelCut(ChannelEnd.AgentFault("an empty message")) to TerminalState.Failed(CAR, "The head unit broke the channel's rules: an empty message"),
            ShellEnd.Failed("This phone's Android lacks a part SSH needs") to TerminalState.Failed(CAR, "This phone's Android lacks a part SSH needs"),
        )
        for ((index, row) in table.withIndex()) {
            val (end, expected) = row
            val shell = rig.opened()
            shell.says("output $index\r\n")
            shell.end(end)
            rig.settle()
            assertEquals("$end", expected, rig.terminal.state.value)
            assertFalse(rig.terminal.state.value.active)
            assertEquals(0, rig.link.inUse.get())
            val words = when (expected) {
                is TerminalState.Ended -> TerminalText.ended(expected.how)
                else -> (expected as TerminalState.Failed).reason
            }
            val tail = listOfNotNull(TerminalLine(TerminalText.NEW_SHELL, mark = true).takeIf { index > 0 }, TerminalLine("output $index"), TerminalLine(words, mark = true))
            assertEquals(tail, rig.lines.takeLast(tail.size))
        }
        assertEquals(table.size * 3 - 1, rig.lines.size)
    }

    @Test
    fun `29 minutes with nothing sent or received close it as idle, and traffic either way puts that off`() = test { rig ->
        val shell = rig.opened()
        advanceTimeBy(20 * MINUTE)
        shell.says("a line\n")
        runCurrent()
        advanceTimeBy(20 * MINUTE)
        rig.terminal.send("ls\n".encodeToByteArray())
        runCurrent()
        // 29 minutes after the send, less a moment: still open.
        advanceTimeBy(TerminalRepository.IDLE_MS - 1)
        runCurrent()
        assertTrue(rig.terminal.state.value is TerminalState.Open)
        assertEquals(0, shell.closes.get())

        advanceTimeBy(2)
        rig.settle()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.Idle), rig.terminal.state.value)
        assertEquals(1, shell.closes.get())
        assertEquals(listOf("ls\n"), shell.written)
        assertEquals(TerminalLine("Closed after 29 minutes with nothing sent or received", mark = true), rig.lines.last())
        assertEquals(1, rig.running())
    }

    @Test
    fun `a job's pause holds the shell, and its end lets it go`() = test { rig ->
        val shell = rig.opened()
        shell.channel.value = ChannelState.Paused
        runCurrent()
        assertEquals(TerminalState.Open(CAR, held = true), rig.terminal.state.value)
        assertTrue(rig.terminal.state.value.active)
        shell.channel.value = ChannelState.Open
        runCurrent()
        assertEquals(TerminalState.Open(CAR, held = false), rig.terminal.state.value)
    }

    @Test
    fun `a lost link's end waits for the reason the head unit's page settles on, with the link no longer held`() = test { rig ->
        val shell = rig.opened()
        rig.link.settles = "This phone's Bluetooth was switched off" to 1_500L
        shell.end(ShellEnd.TunnelCut(ChannelEnd.LinkLost("The head unit closed the link")))
        advanceTimeBy(1_000)
        runCurrent()
        // Still waiting: nothing is said yet.
        assertTrue(rig.terminal.state.value is TerminalState.Open)
        advanceTimeBy(600)
        rig.settle()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.LinkLost("This phone's Bluetooth was switched off")), rig.terminal.state.value)
        assertEquals(TerminalLine("The link to the head unit ended: This phone's Bluetooth was switched off", mark = true), rig.lines.last())
        assertEquals(listOf("The head unit closed the link" to 0), rig.link.asked)

        // Where the reason stays the link's own, so does the end.
        rig.link.settles = null
        rig.terminal.open(CAR, 80, 24)
        rig.settle()
        rig.shell.end(ShellEnd.TunnelCut(ChannelEnd.LinkLost("The head unit closed the link")))
        rig.settle()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.LinkLost("The head unit closed the link")), rig.terminal.state.value)

        // A link lost while the shell opened, the same.
        rig.link.settles = "This phone's Bluetooth was switched off" to 500L
        rig.opener.failure = SshShellException(ShellEnd.TunnelCut(ChannelEnd.LinkLost("The head unit closed the link")))
        rig.terminal.open(CAR, 80, 24)
        advanceTimeBy(600)
        rig.settle()
        assertEquals(TerminalState.Failed(CAR, "This phone's Bluetooth was switched off"), rig.terminal.state.value)
    }

    @Test
    fun `an end that is not the link's asks nothing and waits for nothing`() = test { rig ->
        rig.link.settles = "This phone's Bluetooth was switched off" to 1_500L
        rig.opened().end(ShellEnd.Exited(0))
        runCurrent()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.Exited(0)), rig.terminal.state.value)
        assertTrue(rig.link.asked.isEmpty())
    }

    @Test
    fun `a lost link ends the shell, and the next open connects and opens a new shell below the old lines`() = test { rig ->
        val first = rig.opened()
        first.says("one\r\n")
        runCurrent()
        rig.link.connection.value = ConnectionState.Failed(CAR, "The head unit stopped answering")
        first.end(ShellEnd.TunnelCut(ChannelEnd.LinkLost("The head unit stopped answering")))
        rig.settle()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.LinkLost("The head unit stopped answering")), rig.terminal.state.value)

        rig.terminal.open(CAR, 80, 24)
        rig.settle()
        assertEquals(listOf(CAR), rig.link.connects)
        val second = rig.shell
        assertNotEquals(first, second)
        second.says("two")
        rig.settle()
        assertEquals(
            listOf(
                TerminalLine("one"),
                TerminalLine("The link to the head unit ended: The head unit stopped answering", mark = true),
                TerminalLine(TerminalText.NEW_SHELL, mark = true),
                TerminalLine("two"),
            ),
            rig.lines,
        )
    }

    @Test
    fun `close while connecting gives the connect up, and while the shell opens gives that up, with nothing left`() = test { rig ->
        // Before anything has begun: not even the connect is made.
        rig.terminal.open(CAR, 80, 24)
        rig.terminal.close()
        rig.settle()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.ByUser), rig.terminal.state.value)
        assertEquals(emptyList<String>(), rig.link.connects)
        assertEquals(1, rig.running())

        rig.link.gate = CompletableDeferred()
        rig.terminal.open(CAR, 80, 24)
        runCurrent()
        rig.terminal.close()
        rig.settle()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.ByUser), rig.terminal.state.value)
        assertEquals(1, rig.running())
        rig.link.gate = null

        rig.opener.gate = CompletableDeferred()
        rig.terminal.open(CAR, 80, 24)
        runCurrent()
        assertEquals(TerminalState.Connecting(CAR), rig.terminal.state.value)
        assertEquals(1, rig.link.inUse.get())
        rig.terminal.close()
        rig.settle()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.ByUser), rig.terminal.state.value)
        assertEquals(1, rig.opener.gaveUp.get())
        assertEquals(0, rig.link.inUse.get())
        assertEquals(1, rig.running())
        assertEquals(0, rig.opener.opened.size)
        // No shell was shown open, so no line was added.
        assertEquals(emptyList<TerminalLine>(), rig.lines)
    }

    @Test
    fun `a second open while one is connecting or open starts no second shell`() = test { rig ->
        rig.terminal.open(CAR, 80, 24)
        rig.terminal.open(CAR, 80, 24)
        rig.terminal.open("other", 80, 24)
        rig.settle()
        rig.terminal.open(CAR, 80, 24)
        rig.terminal.open("other", 80, 24)
        rig.settle()
        assertEquals(listOf(CAR), rig.link.connects)
        assertEquals(1, rig.opener.opened.size)
        assertEquals(TerminalState.Open(CAR), rig.terminal.state.value)
        assertEquals(1, rig.kept.get())
    }

    @Test
    fun `the lines are given out about 20 times a second, and the reader never waits for them`() = test { rig ->
        rig.opener.capacity = 1
        val shell = rig.opened()
        val seen = CopyOnWriteArrayList<TerminalSnapshot>()
        backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) { rig.terminal.lines.collect { seen += it } }

        // A thousand pieces at once, through a shell that holds one: all read without the clock moving.
        val began = currentTime
        val writer = launch { repeat(1_000) { shell.output.send("$it\n".encodeToByteArray()) } }
        runCurrent()
        assertTrue(writer.isCompleted)
        assertEquals(began, currentTime)
        assertTrue("${seen.size}", seen.size <= 3)
        advanceTimeBy(TerminalRepository.PUBLISH_EVERY_MS)
        runCurrent()
        assertEquals("999", rig.texts.last())

        // A piece every millisecond for a second.
        seen.clear()
        val steady = launch {
            repeat(1_000) {
                shell.output.send("more $it\n".encodeToByteArray())
                delay(1)
            }
        }
        advanceTimeBy(1_000)
        runCurrent()
        assertTrue(steady.isCompleted)
        assertTrue("${seen.size} in a second", seen.size in 19..21)
        rig.settle()
        assertEquals("more 999", rig.texts.last())
    }

    @Test
    fun `sizes are told to the shell once they stop changing, and what is typed goes in order`() = test { rig ->
        val shell = rig.opened(80, 24)
        repeat(50) {
            rig.terminal.resize(81 + it, 24 + it % 3)
            advanceTimeBy(10)
        }
        runCurrent()
        assertEquals(emptyList<Pair<Int, Int>>(), shell.sizes)
        advanceTimeBy(TerminalRepository.RESIZE_AFTER_MS + 1)
        runCurrent()
        assertEquals(listOf(130 to 25), shell.sizes)

        // Back to what it was and then to that again within the wait: nothing new to tell.
        rig.terminal.resize(80, 24)
        advanceTimeBy(10)
        rig.terminal.resize(130, 25)
        advanceTimeBy(TerminalRepository.RESIZE_AFTER_MS + 1)
        runCurrent()
        assertEquals(listOf(130 to 25), shell.sizes)

        rig.terminal.send("echo one\n".encodeToByteArray())
        rig.terminal.send("echo two\n".encodeToByteArray())
        rig.terminal.send(ByteArray(0))
        runCurrent()
        assertEquals(listOf("echo one\n", "echo two\n"), shell.written)

        // The next shell opens with the last size.
        rig.terminal.close()
        rig.settle()
        rig.terminal.open(CAR, 0, 0)
        rig.settle()
        assertEquals(130 to 25, rig.opener.sizes.last())
    }

    @Test
    fun `nothing is sent where no shell is open, and nothing is left running after an end`() = test { rig ->
        rig.terminal.send("early\n".encodeToByteArray())
        rig.link.gate = CompletableDeferred()
        rig.terminal.open(CAR, 80, 24)
        runCurrent()
        rig.terminal.send("connecting\n".encodeToByteArray())
        rig.link.gate!!.complete(Unit)
        rig.settle()
        val shell = rig.shell
        assertEquals(emptyList<String>(), shell.written)
        assertTrue(rig.running() > 1)

        rig.terminal.close()
        rig.terminal.send("closing\n".encodeToByteArray())
        rig.settle()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.ByUser), rig.terminal.state.value)
        assertEquals(1, shell.closes.get())
        assertEquals(TerminalLine("Closed", mark = true), rig.lines.last())
        assertEquals(1, rig.running())
        assertEquals(0, rig.link.inUse.get())
        rig.terminal.send("after\n".encodeToByteArray())
        runCurrent()
        assertEquals(emptyList<String>(), shell.written)
        // A close with nothing open does nothing.
        rig.terminal.close()
        runCurrent()
        assertEquals(TerminalState.Ended(CAR, TerminalEnd.ByUser), rig.terminal.state.value)
    }

    @Test
    fun `clear empties the lines but the prompt, and an open on another head unit begins with none`() = test { rig ->
        val shell = rig.opened()
        shell.says("one\r\ntwo\r\n$ ")
        rig.settle()
        val before = rig.terminal.lines.value.revision
        rig.terminal.clear()
        rig.settle()
        // The prompt stays, and Copy all gives what is shown.
        assertEquals(listOf("$ "), rig.texts)
        assertEquals("$ ", rig.terminal.text())
        assertNotEquals(before, rig.terminal.lines.value.revision)
        // The echo goes on in the prompt's line.
        shell.says("ls\r\nthree\r\n$ ")
        rig.settle()
        assertEquals(listOf("$ ls", "three", "$ "), rig.texts)

        // Once the shell has ended, its lines are all finished: Clear leaves nothing.
        rig.terminal.close()
        rig.settle()
        rig.terminal.clear()
        rig.settle()
        assertEquals(emptyList<TerminalLine>(), rig.lines)
        assertEquals("", rig.terminal.text())

        // An open on another head unit drops the line being written too.
        rig.terminal.open(CAR, 80, 24)
        rig.settle()
        rig.shell.says("$ ")
        rig.shell.end(ShellEnd.ServerClosed)
        rig.settle()
        assertTrue(rig.lines.isNotEmpty())
        rig.link.connection.value = connected("other")
        rig.terminal.open("other", 80, 24)
        rig.settle()
        assertEquals(TerminalState.Open("other"), rig.terminal.state.value)
        assertEquals(emptyList<TerminalLine>(), rig.lines)
        rig.shell.says("x")
        rig.settle()
        assertEquals(listOf("x"), rig.texts)
    }

    @Test
    fun `the service is told at each open, and the state says when it is to run`() = test { rig ->
        assertFalse(rig.terminal.state.value.active)
        rig.opened()
        assertEquals(1, rig.kept.get())
        assertTrue(rig.terminal.state.value.active)
        rig.terminal.close()
        rig.settle()
        assertFalse(rig.terminal.state.value.active)
        rig.link.channels = false
        rig.link.connection.value = ConnectionState.Disconnected
        rig.terminal.open(CAR, 80, 24)
        assertEquals(2, rig.kept.get())
        rig.settle()
        assertFalse(rig.terminal.state.value.active)
    }

    private companion object {
        const val CAR = "car"
        const val MINUTE = 60_000L
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.terminal

import android.view.WindowManager
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.TextFieldValue
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.MemoryLink
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.ssh.ShellEnd
import com.nbtapps.store.data.ssh.SshTunnelException
import com.nbtapps.store.data.ssh.TunnelFailure
import com.nbtapps.store.data.terminal.ShellOpener
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalLink
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.terminal.TerminalShell
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.data.terminal.TerminalText
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.ChannelSwitch
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.withTestTimeout
import java.io.IOException
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/**
 * The terminal screen's view model over the app's real session holder ([TerminalRepository]), whose link and shells
 * are stand-ins: what each state shows, the open as the screen comes up, Send and the keys, the history, the size,
 * Close, Clear and Copy all, and Switch on.
 */
class TerminalViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val sessionScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val session = AgentSession(MemoryLink.pair().first, sessionScope)

    @After
    fun tearDown() {
        session.close("the test is over")
        sessionScope.cancel()
    }

    /** The app's link, connected to whichever head unit is asked for. */
    private inner class FakeLink : TerminalLink {
        override val connection = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)

        override suspend fun connect(headUnitId: String): Result<Unit> {
            connection.value = ConnectionState.Connected(
                headUnitId,
                TcpEndpoint("192.0.2.1", 7144),
                AgentInfo("1.7.0", "000102030405060708090a0b0c0d0e0f", 1),
                emptyList(),
                capabilities = listOf(ProtocolConstants.CAPABILITY_CHANNELS),
            )
            return Result.success(Unit)
        }

        override suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T> = try {
            Result.success(block(session))
        } catch (e: IOException) {
            Result.failure(e)
        }
    }

    /** A shell the test drives: what it is sent, its sizes, what it writes, how it ends. */
    private class FakeShell : TerminalShell {
        override val channel = MutableStateFlow<ChannelState>(ChannelState.Open)
        private val done = CompletableDeferred<ShellEnd>()
        override val ended: Deferred<ShellEnd> get() = done
        val output = Channel<ByteArray>(Channel.UNLIMITED)
        val written = CopyOnWriteArrayList<ByteArray>()
        val sizes = CopyOnWriteArrayList<Pair<Int, Int>>()

        override suspend fun read(): ByteArray? = output.receiveCatching().getOrNull()

        override suspend fun write(bytes: ByteArray) {
            written += bytes
        }

        override suspend fun resize(columns: Int, rows: Int) {
            sizes += columns to rows
        }

        override suspend fun close(): ShellEnd {
            end(ShellEnd.ByApp)
            return done.await()
        }

        fun says(text: String) = output.trySend(text.encodeToByteArray())

        fun end(how: ShellEnd) {
            output.close()
            done.complete(how)
        }

        /** What was sent, each piece as text. */
        val sent: List<String> get() = written.map { it.decodeToString() }
    }

    private class FakeOpener : ShellOpener {
        val opened = CopyOnWriteArrayList<FakeShell>()
        val sizes = CopyOnWriteArrayList<Pair<Int, Int>>()

        @Volatile
        var failure: Exception? = null

        @Volatile
        var gate: CompletableDeferred<Unit>? = null

        override suspend fun open(session: AgentSession, columns: Int, rows: Int, scope: CoroutineScope): TerminalShell {
            sizes += columns to rows
            gate?.await()
            failure?.let { throw it }
            return FakeShell().also { opened += it }
        }
    }

    private val link = FakeLink()
    private val opener = FakeOpener()
    private val terminal = TerminalRepository(link, opener, {}, scope)
    private val history = CommandHistory()
    private val switches = CopyOnWriteArrayList<Boolean>()

    @Volatile
    private var switchAnswer: Result<ChannelSwitch> = Result.success(ChannelSwitch(switched = true, on = true, open = 0))

    private fun viewModel(headUnitId: String = "car", name: Flow<String?> = flowOf("Test car")) = mainDispatcherRule.track(
        TerminalViewModel(headUnitId, terminal, history, name) { on ->
            switches += on
            switchAnswer
        },
    )

    private val shell: FakeShell get() = opener.opened.last()

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTestTimeout(30_000, block) }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private suspend fun TerminalViewModel.view(what: String, predicate: (TerminalView) -> Boolean): TerminalUiState =
        await(uiState, what) { predicate(it.view) }

    private suspend fun TerminalViewModel.opened(): TerminalUiState = view("the shell open") { it == TerminalView.Open(held = false) }

    /** Waits until the shell has been sent [count] pieces. */
    private suspend fun sentCount(count: Int) {
        withTimeoutOrNull(5_000) { while (shell.written.size < count) delay(5) } ?: throw AssertionError("sent ${shell.sent}")
    }

    // ---- The states ----

    @Test
    fun `each state of the terminal is shown as its own`() {
        assertEquals(TerminalView.Opening, terminalView(TerminalState.Idle, "car"))
        assertEquals(TerminalView.Opening, terminalView(TerminalState.Connecting("car"), "car"))
        assertEquals(TerminalView.Open(held = false), terminalView(TerminalState.Open("car"), "car"))
        assertEquals(TerminalView.Open(held = true), terminalView(TerminalState.Open("car", held = true), "car"))
        assertEquals(TerminalView.Ended, terminalView(TerminalState.Ended("car", TerminalEnd.Exited(0)), "car"))
        assertEquals(TerminalView.Failed("no answer"), terminalView(TerminalState.Failed("car", "no answer"), "car"))
        assertEquals(TerminalView.Off, terminalView(TerminalState.Off("car"), "car"))
        assertEquals(TerminalView.NoPackage, terminalView(TerminalState.NoPackage("car"), "car"))
        assertEquals(TerminalView.Unsupported, terminalView(TerminalState.Unsupported("car"), "car"))
        // Another head unit's: one open there is in the way; one that is over is the end of that, met only after it.
        assertEquals(TerminalView.Elsewhere, terminalView(TerminalState.Open("other"), "car"))
        assertEquals(TerminalView.Elsewhere, terminalView(TerminalState.Connecting("other"), "car"))
        assertEquals(TerminalView.OtherClosed, terminalView(TerminalState.Ended("other", TerminalEnd.ByUser), "car"))
        assertEquals(TerminalView.OtherClosed, terminalView(TerminalState.Failed("other", "no"), "car"))
    }

    @Test
    fun `the keys, the field and Send are for an open shell that is not held, and the bar's actions for one with lines`() {
        fun ui(view: TerminalView) = TerminalUiState(view = view)
        assertTrue(ui(TerminalView.Open(held = false)).canType)
        listOf(TerminalView.Opening, TerminalView.Open(held = true), TerminalView.Ended, TerminalView.Off).forEach { assertFalse(it.toString(), ui(it).canType) }
        listOf(TerminalView.Opening, TerminalView.Open(held = false), TerminalView.Open(held = true), TerminalView.Ended).forEach {
            assertTrue(it.toString(), ui(it).hasActions)
        }
        listOf(TerminalView.Failed("x"), TerminalView.Off, TerminalView.NoPackage, TerminalView.Unsupported, TerminalView.Elsewhere, TerminalView.OtherClosed).forEach {
            assertFalse(it.toString(), ui(it).hasActions)
        }
        assertTrue(ui(TerminalView.Opening).canClose)
        assertTrue(ui(TerminalView.Open(held = true)).canClose)
        assertFalse(ui(TerminalView.Ended).canClose)
    }

    @Test
    fun `the screen shows the head unit's name, the opening, the open shell, the hold and the end`() = test {
        opener.gate = CompletableDeferred()
        val viewModel = viewModel()
        assertEquals("Test car", await(viewModel.uiState, "the name") { it.name != null }.name)
        viewModel.view("opening") { it == TerminalView.Opening }

        requireNotNull(opener.gate).complete(Unit)
        viewModel.opened()
        shell.channel.value = ChannelState.Paused
        viewModel.view("held") { it == TerminalView.Open(held = true) }
        shell.channel.value = ChannelState.Open
        viewModel.opened()

        shell.end(ShellEnd.Exited(0))
        val ended = viewModel.view("ended") { it == TerminalView.Ended }
        // The closing line is the app's own, dimmed.
        await(viewModel.uiState, "the closing line") { it.lines.lines.lastOrNull()?.mark == true }
        assertEquals(TerminalText.ended(TerminalEnd.Exited(0)), viewModel.uiState.value.lines.lines.last().text)
        assertFalse(ended.canType)
    }

    @Test
    fun `a refusal is shown as its state, off, not installed, too old or failed`() = test {
        opener.failure = SshTunnelException(TunnelFailure.Refused("off", "off"))
        val viewModel = viewModel()
        viewModel.view("off") { it == TerminalView.Off }

        opener.failure = SshTunnelException(TunnelFailure.Refused("not-found", "not-found"))
        viewModel.open()
        viewModel.view("not installed") { it == TerminalView.NoPackage }

        opener.failure = SshTunnelException(TunnelFailure.Refused("unsupported", "unsupported"))
        viewModel.open()
        viewModel.view("too old") { it == TerminalView.Unsupported }

        opener.failure = SshTunnelException(TunnelFailure.NoAnswer(toKeys = true))
        viewModel.open()
        val failed = viewModel.view("failed") { it is TerminalView.Failed }.view as TerminalView.Failed
        assertEquals("The SSH package on the head unit didn't answer in time", failed.reason)

        // Try again.
        opener.failure = null
        viewModel.open()
        viewModel.opened()
    }

    // ---- Opening as the screen comes up ----

    @Test
    fun `coming up opens a shell unless one is open or being opened on this head unit`() = test {
        opener.gate = CompletableDeferred()
        viewModel()
        await(terminal.state, "connecting") { it is TerminalState.Connecting }
        // One being opened: a second screen opens nothing more.
        viewModel()
        requireNotNull(opener.gate).complete(Unit)
        await(terminal.state, "open") { it is TerminalState.Open }
        assertEquals(1, opener.sizes.size)

        // One open: nothing more either.
        viewModel().opened()
        assertEquals(1, opener.sizes.size)

        // Once it has ended, coming up opens a new one.
        terminal.close()
        await(terminal.state, "ended") { it is TerminalState.Ended }
        viewModel().opened()
        assertEquals(2, opener.opened.size)
    }

    @Test
    fun `one open on another head unit is left alone, and this screen says so`() = test {
        viewModel("other").opened()
        val here = viewModel("car")
        here.view("elsewhere") { it == TerminalView.Elsewhere }
        assertEquals(1, opener.opened.size)
        // Close here closes nothing of the other's.
        here.close()
        delay(100)
        assertEquals(TerminalState.Open("other"), terminal.state.value)
    }

    @Test
    fun `once the other head unit's terminal ends, this screen shows none of its lines, opens nothing by itself, and opens here when asked`() = test {
        val there = viewModel("other")
        there.opened()
        shell.says("$ uname -s\r\nQNX\r\n$ ")
        await(there.uiState, "the other's lines") { it.lines.lines.size == 3 }
        val here = viewModel("car")
        assertTrue(here.view("elsewhere") { it == TerminalView.Elsewhere }.lines.lines.isEmpty())

        shell.end(ShellEnd.Exited(0))
        val closed = here.view("the other closed") { it == TerminalView.OtherClosed }
        assertTrue(closed.lines.lines.isEmpty())
        assertFalse(closed.hasActions)
        // Nothing is opened here by itself.
        delay(200)
        assertEquals(TerminalView.OtherClosed, here.uiState.value.view)
        assertEquals(1, opener.opened.size)

        // Copy all and Clear act on none of the other's lines from here.
        assertEquals("", here.copyText())
        here.clear()
        delay(2 * TerminalRepository.PUBLISH_EVERY_MS)
        assertTrue(terminal.lines.value.lines.isNotEmpty())

        // Open terminal: a shell here, whose lines begin anew.
        here.open()
        here.opened()
        assertEquals(2, opener.opened.size)
        assertEquals(TerminalState.Open("car"), terminal.state.value)
        delay(2 * TerminalRepository.PUBLISH_EVERY_MS)
        assertEquals(emptyList<String>(), here.uiState.value.lines.lines.map { it.text })
    }

    @Test
    fun `coming up never shows the other head unit's terminal as closed`() = test {
        viewModel("other").opened()
        shell.says("$ ")
        terminal.close()
        await(terminal.state, "the other ended") { it is TerminalState.Ended }

        opener.gate = CompletableDeferred()
        val here = viewModel("car")
        val seen = CopyOnWriteArrayList<TerminalView>()
        seen += here.uiState.value.view
        val watch = launch { here.uiState.collect { seen += it.view } }
        requireNotNull(opener.gate).complete(Unit)
        here.opened()
        watch.cancel()
        assertFalse(seen.toString(), TerminalView.OtherClosed in seen)
        assertFalse(seen.toString(), TerminalView.Elsewhere in seen)
        assertEquals(TerminalView.Opening, seen.first())
    }

    // ---- Send and the keys ----

    @Test
    fun `Send sends the field's text and Enter, empties the field, and leaves the echo to the shell`() = test {
        val viewModel = viewModel()
        viewModel.opened()
        val before = viewModel.uiState.value.lines

        viewModel.type(TextFieldValue("ls /tmp"))
        viewModel.send()
        sentCount(1)
        assertEquals(listOf("ls /tmp\r"), shell.sent)
        assertEquals("", viewModel.field.text)
        // The line shows only as the shell echoes it.
        delay(2 * TerminalRepository.PUBLISH_EVERY_MS)
        assertEquals(before.lines, viewModel.uiState.value.lines.lines)
        shell.says("ls /tmp\r\nagent.log\r\n")
        await(viewModel.uiState, "the echo") { ui -> ui.lines.lines.any { it.text == "ls /tmp" } }

        // An empty field sends Enter alone.
        viewModel.send()
        sentCount(2)
        assertEquals("\r", shell.sent[1])
    }

    @Test
    fun `the control keys send their byte at once, whatever is in the field`() = test {
        val viewModel = viewModel()
        viewModel.opened()
        viewModel.type(TextFieldValue("sleep 600"))

        viewModel.key(ControlKey.CTRL_C)
        viewModel.key(ControlKey.CTRL_D)
        viewModel.key(ControlKey.CTRL_Z)
        sentCount(3)
        assertArrayEquals(byteArrayOf(0x03), shell.written[0])
        assertArrayEquals(byteArrayOf(0x04), shell.written[1])
        assertArrayEquals(byteArrayOf(0x1A), shell.written[2])
        assertEquals("sleep 600", viewModel.field.text)
    }

    @Test
    fun `nothing is sent while a job holds the shell`() = test {
        val viewModel = viewModel()
        viewModel.opened()
        shell.channel.value = ChannelState.Paused
        viewModel.view("held") { it == TerminalView.Open(held = true) }

        viewModel.type(TextFieldValue("ls"))
        viewModel.send()
        viewModel.key(ControlKey.CTRL_C)
        delay(100)
        assertTrue(shell.written.isEmpty())
        assertEquals("ls", viewModel.field.text)
    }

    // ---- The history ----

    @Test
    fun `Up and Down bring back the user's own earlier lines, and what was being typed`() = test {
        val viewModel = viewModel()
        viewModel.opened()
        for (line in listOf("uname -s", "ls /tmp", "pidin")) {
            viewModel.type(TextFieldValue(line))
            viewModel.send()
        }
        viewModel.type(TextFieldValue("df"))

        viewModel.earlier()
        assertEquals(TextFieldValue("pidin", TextRange(5)), viewModel.field)
        viewModel.earlier()
        assertEquals("ls /tmp", viewModel.field.text)
        viewModel.earlier()
        assertEquals("uname -s", viewModel.field.text)
        // The oldest stays.
        viewModel.earlier()
        assertEquals("uname -s", viewModel.field.text)

        viewModel.later()
        assertEquals("ls /tmp", viewModel.field.text)
        viewModel.later()
        assertEquals("pidin", viewModel.field.text)
        // Past the newest: what was being typed.
        viewModel.later()
        assertEquals("df", viewModel.field.text)
        viewModel.later()
        assertEquals("df", viewModel.field.text)
        // Up and Down send nothing.
        sentCount(3)
        delay(50)
        assertEquals(3, shell.written.size)
    }

    @Test
    fun `a line brought back and edited leaves the history as it was, and is the draft the walk comes back to`() = test {
        val viewModel = viewModel()
        viewModel.opened()
        for (line in listOf("ls /tmp", "pidin")) {
            viewModel.type(TextFieldValue(line))
            viewModel.send()
        }

        viewModel.earlier()
        viewModel.earlier()
        assertEquals("ls /tmp", viewModel.field.text)
        viewModel.type(TextFieldValue("ls /var", TextRange(7)))
        // The walk begins again from the newest line, and comes back down to the edit.
        viewModel.earlier()
        assertEquals("pidin", viewModel.field.text)
        viewModel.later()
        assertEquals("ls /var", viewModel.field.text)
        assertEquals(listOf("ls /tmp", "pidin"), history.all)

        // Moving the cursor alone is no edit.
        viewModel.earlier()
        viewModel.type(TextFieldValue("pidin", TextRange(2)))
        viewModel.earlier()
        assertEquals("ls /tmp", viewModel.field.text)

        // Sent, the edit is a line of its own.
        viewModel.type(TextFieldValue("ls /var"))
        viewModel.send()
        assertEquals(listOf("ls /tmp", "pidin", "ls /var"), history.all)
    }

    @Test
    fun `the history keeps the last 50 lines, and no empty line or line twice in a row`() {
        val history = CommandHistory()
        repeat(60) { history.sent("echo $it") }
        assertEquals(CommandHistory.LIMIT, history.all.size)
        assertEquals("echo 10", history.all.first())
        assertEquals("echo 59", history.all.last())

        history.sent("")
        history.sent("echo 59")
        assertEquals("echo 59", history.all.last())
        assertEquals(50, history.all.size)

        var oldest: String? = null
        while (true) oldest = history.earlier("") ?: break
        assertEquals("echo 10", oldest)
        assertNull(CommandHistory().earlier("x"))
        assertNull(CommandHistory().later())
    }

    // ---- The size ----

    @Test
    fun `the size is as many whole characters as the lines area holds`() {
        assertEquals(TerminalGrid(80, 24), terminalGrid(800, 432, 10f, 18f))
        assertEquals(TerminalGrid(79, 23), terminalGrid(799, 431, 10f, 18f))
        assertEquals(TerminalGrid(47, 30), terminalGrid(1000, 1440, 21.25f, 47.25f))
        // A keyboard that leaves little room still leaves one column, and the shell is told of 4 rows at least.
        assertEquals(TerminalGrid(1, 4), terminalGrid(5, 0, 10f, 18f))
        assertEquals(TerminalGrid(80, 4), terminalGrid(800, 18, 10f, 18f))
        assertEquals(TerminalGrid(80, 5), terminalGrid(800, 90, 10f, 18f))
        assertNull(terminalGrid(800, 432, 0f, 18f))
    }

    @Test
    fun `the layout is compact where the height left is short, whatever the orientation`() {
        // A phone in portrait, the keyboard down and up.
        assertFalse(compactTerminal(heightDp = 915f, topDp = 24f, bottomDp = 48f, imeDp = 0f))
        assertFalse(compactTerminal(heightDp = 915f, topDp = 24f, bottomDp = 48f, imeDp = 330f))
        // The same phone in landscape: the keyboard down, and up.
        assertFalse(compactTerminal(heightDp = 411f, topDp = 24f, bottomDp = 48f, imeDp = 0f))
        assertTrue(compactTerminal(heightDp = 411f, topDp = 24f, bottomDp = 48f, imeDp = 200f))
        // A small phone with the keyboard up, and the lower half of a split screen.
        assertTrue(compactTerminal(heightDp = 640f, topDp = 24f, bottomDp = 48f, imeDp = 300f))
        assertTrue(compactTerminal(heightDp = 300f, topDp = 0f, bottomDp = 0f, imeDp = 0f))
        // The edge.
        assertFalse(compactTerminal(heightDp = 320f, topDp = 0f, bottomDp = 0f, imeDp = 0f))
        assertTrue(compactTerminal(heightDp = 319f, topDp = 0f, bottomDp = 0f, imeDp = 0f))
    }

    @Test
    fun `leaving the terminal gives the window back its soft input mode, pan where it had none`() {
        val hidden = WindowManager.LayoutParams.SOFT_INPUT_STATE_HIDDEN
        assertEquals(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN, restoredSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_UNSPECIFIED))
        assertEquals(hidden or WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN, restoredSoftInputMode(hidden))
        assertEquals(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE, restoredSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE))
        assertEquals(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING, restoredSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING))
    }

    @Test
    fun `the size goes to the shell when the area changes`() = test {
        val viewModel = viewModel()
        viewModel.opened()
        viewModel.sized(TerminalGrid(47, 30))
        // The keyboard comes up.
        viewModel.sized(TerminalGrid(47, 14))
        withTimeoutOrNull(5_000) { while (shell.sizes.isEmpty()) delay(10) }
        assertEquals(listOf(47 to 14), shell.sizes)

        // A shell opened after is opened with it.
        terminal.close()
        await(terminal.state, "ended") { it is TerminalState.Ended }
        viewModel.open()
        viewModel.opened()
        assertEquals(47 to 14, opener.sizes.last())
    }

    // ---- Close, Clear, Copy all ----

    @Test
    fun `Close ends the shell and the screen stays, Clear empties the lines, Copy all is every line`() = test {
        val viewModel = viewModel()
        viewModel.opened()
        shell.says("# uname -s\r\nQNX\r\n# ")
        await(viewModel.uiState, "the lines") { it.lines.lines.size == 3 }
        assertEquals("# uname -s\nQNX\n# ", viewModel.copyText())

        viewModel.clear()
        // The prompt stays, and Copy all gives what is shown.
        await(viewModel.uiState, "the prompt alone") { ui -> ui.lines.lines.map { it.text } == listOf("# ") }
        assertEquals("# ", viewModel.copyText())

        viewModel.close()
        viewModel.view("ended") { it == TerminalView.Ended }
        assertEquals(TerminalState.Ended("car", TerminalEnd.ByUser), terminal.state.value)
        // Open again.
        viewModel.open()
        viewModel.opened()
        assertEquals(2, opener.opened.size)
    }

    // ---- Switch on ----

    @Test
    fun `Switch on asks first, switches SSH access on and opens a shell, and Cancel does nothing`() = test {
        opener.failure = SshTunnelException(TunnelFailure.Refused("off", "off"))
        val viewModel = viewModel()
        viewModel.view("off") { it == TerminalView.Off }

        viewModel.askOn()
        await(viewModel.uiState, "the question") { it.confirmingOn }
        viewModel.cancelOn()
        await(viewModel.uiState, "put away") { !it.confirmingOn }
        assertTrue(switches.isEmpty())

        opener.failure = null
        viewModel.askOn()
        viewModel.confirmOn()
        viewModel.opened()
        assertEquals(listOf(true), switches)
        assertNull(viewModel.uiState.value.switchError)
    }

    @Test
    fun `a refusal of the switch is told under it, and no shell is opened`() = test {
        opener.failure = SshTunnelException(TunnelFailure.Refused("off", "off"))
        val viewModel = viewModel()
        viewModel.view("off") { it == TerminalView.Off }
        val tries = opener.sizes.size
        switchAnswer = Result.failure(AgentRefusal("busy", "busy"))

        viewModel.askOn()
        viewModel.confirmOn()
        val refused = await(viewModel.uiState, "the refusal") { it.switchError != null && !it.switchingOn }
        assertEquals("The head unit is busy with a job. Try again once it has finished.", refused.switchError)
        assertEquals(TerminalView.Off, refused.view)
        assertEquals(tries, opener.sizes.size)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.AgentBench
import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.terminal.JobTerminalLink
import com.nbtapps.store.data.terminal.SshShellOpener
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalLine
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.data.terminal.TerminalText
import com.nbtapps.store.model.ChannelCloseReason
import com.nbtapps.store.model.ChannelError
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.TcpEndpoint
import com.jcraft.jsch.ChannelExec
import com.jcraft.jsch.JSch
import java.io.File
import java.io.InputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.util.Base64
import java.io.OutputStream
import java.net.UnixDomainSocketAddress
import java.nio.ByteBuffer
import java.nio.channels.SocketChannel
import java.nio.file.Files
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * The tunnel and the shell against the real agent and the SSH package's bench form (`headunit/tests/packages/ssh-1.0.0`):
 * the agent repo's `nbtagent` in its container ([AgentBench]), which starts the bench's relay, which starts Dropbear
 * for each connection, all built from the same sources as the head unit's. The package is installed as
 * [com.nbtapps.store.data.jobs.RealAgentTest] installs: its `requires.app` names a later app than this one, which only
 * the catalogue's resolver looks at. On the bench the server logs in the user the agent runs as, `bench`, whose shell
 * is `/bin/sh`. Nothing here goes near a car.
 *
 * The terminal's session holder ([TerminalRepository]) is run here too, over the app's link, this tunnel and this shell.
 */
class SshRealAgentTest {

    private lateinit var work: File
    private lateinit var socket: File
    private var agent: Process? = null

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    /** The app's link to the bench, once [installed] has made it. */
    private lateinit var jobs: JobRepository
    private val links = java.util.concurrent.CopyOnWriteArrayList<SocketLink>()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Unit test phone"
    }

    /** A link over the agent's local socket, which the agent trusts as it is: no `auth` is asked on it. */
    private class SocketLink(path: File) : Link {
        private val channel = SocketChannel.open(UnixDomainSocketAddress.of(path.toPath()))
        override val kind = "local"

        override val input = object : InputStream() {
            override fun read(): Int = throw UnsupportedOperationException()
            override fun read(buffer: ByteArray, offset: Int, length: Int): Int = channel.read(ByteBuffer.wrap(buffer, offset, length))
        }

        override val output = object : OutputStream() {
            override fun write(byte: Int) = write(byteArrayOf(byte.toByte()), 0, 1)
            override fun write(bytes: ByteArray, offset: Int, length: Int) {
                val buffer = ByteBuffer.wrap(bytes, offset, length)
                while (buffer.hasRemaining()) channel.write(buffer)
            }
        }

        override fun close() = channel.close()
    }

    private val connector = object : LinkConnector {
        override fun supports(endpoint: Endpoint) = true
        override suspend fun connect(endpoint: Endpoint): Link = SocketLink(socket).also { links += it }
    }

    @Before
    fun startAgent() {
        AgentBench.need(AgentBench.MISSING, AgentBench.available)
        // A short path: a local socket's name has to fit in about a hundred bytes.
        work = Files.createTempDirectory("nbt").toFile()
        socket = File(work, "s/a")
        agent = AgentBench.start(File(work, "root").apply { mkdirs() }, socket, File(work, "agent.log"))
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
        while (!socket.exists()) {
            check(System.nanoTime() < deadline && agent!!.isAlive) { "the agent did not start:\n" + File(work, "agent.log").readText() }
            Thread.sleep(20)
        }
    }

    @After
    fun tearDown() {
        scope.cancel()
        links.forEach { runCatching { it.close() } }
        agent?.let { AgentBench.stop(it) }
        if (::work.isInitialized) work.deleteRecursively()
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTimeout(120_000) { block() } }

    /** Installs the SSH package's bench form through a job, as the app does, and returns a link of the test's own with the switch on. */
    private suspend fun installed(): AgentSession {
        val headUnits = InMemoryHeadUnitRepository()
        headUnits.add(HeadUnit("bench", "Test bench", listOf(TcpEndpoint("local", 1))))
        val store = PackageStore(File(work, "phone")).apply { load() }
        jobs = JobRepository(headUnits, connector, identity, MemoryPairingKeys(), store, {}, scope, shareDir = File(work, "share"))
        assertTrue(jobs.connect("bench").isSuccess)
        assertTrue("the agent has channels", (jobs.connection.value as ConnectionState.Connected).channels)
        val ssh = AgentVectors.file("packages/good/org.nbtapp.ssh-1.0.0.nbtpkg").inputStream().use { store.add(it, listOf(AgentVectors.packageKey)) }
        val plan = jobs.plan(listOf(InstallTarget(SshTunnel.PACKAGE, "1.0.0")), emptyList(), listOf(ssh)).getOrThrow()
        assertTrue("${plan.missing} ${plan.problems}", plan.runnable)
        jobs.start(plan).getOrThrow()
        val ended = withTimeoutOrNull(60_000) { jobs.job.first { it != null && it.planId == plan.planId && it.ended } }
        assertEquals(JobState.SUCCEEDED, ended?.job?.state)

        val session = AgentSession(connector.connect(TcpEndpoint("local", 1)), scope)
        session.client.hello(identity.id(), identity.name)
        assertEquals(ChannelError.OFF, runCatching { SshTunnel.open(session, listOf(SshKey.generate().publicKeyLine())) }
            .exceptionOrNull().let { (it as SshTunnelException).failure as TunnelFailure.Refused }.code)
        assertTrue(session.client.channelSet(SshTunnel.PACKAGE, SshTunnel.CHANNEL, true).on)
        return session
    }

    /** What the shell has written so far, read as it comes. */
    private class Screen(shell: SshShell, scope: CoroutineScope) {
        private val text = StringBuilder()

        init {
            scope.launch {
                while (true) {
                    val piece = shell.read() ?: break
                    synchronized(text) { text.append(piece.decodeToString()) }
                }
            }
        }

        val now: String get() = synchronized(text) { text.toString() }

        /** Waits for [pattern] to show in what came after [from], and returns its match. */
        suspend fun await(pattern: Regex, from: Int = 0): MatchResult {
            val found = withTimeoutOrNull(20_000) {
                var match = pattern.find(now, from)
                while (match == null) {
                    delay(20)
                    match = pattern.find(now, from)
                }
                match
            }
            return found ?: throw AssertionError("never came: $pattern; the shell wrote:\n$now")
        }
    }

    private suspend fun SshShell.type(line: String) = write("$line\n".encodeToByteArray())

    /** No thread of JSch's is left. */
    private fun assertNoJschThread() {
        val left = Thread.getAllStackTraces().keys.filter { it.isAlive && (it.name.startsWith("Connect thread") || it.name.startsWith("Shell for")) }
        assertTrue("left: $left", left.isEmpty())
    }

    @Test
    fun `a login with the app's key, a command and its output, the pty's size before and after a resize, and the exit status`() = test {
        val session = installed()
        val key = SshKey.generate()
        val tunnel = SshTunnel.open(session, listOf(key.publicKeyLine()))
        assertTrue(tunnel.pty)
        assertEquals(1, tunnel.hostKeys.size)
        assertTrue(tunnel.hostKeys.single().startsWith("ssh-ed25519 "))

        val shell = SshShell.open(tunnel, key, USER, columns = 80, rows = 24, scope = scope)
        val screen = Screen(shell, scope)
        shell.type("echo hello-$((6 * 7)) \$TERM")
        screen.await(Regex("hello-42 dumb"))
        shell.type("stty size; echo sized")
        screen.await(Regex("24 80\\s+sized"))
        val before = screen.now.length
        shell.resize(columns = 100, rows = 30)
        shell.type("stty size; echo resized")
        screen.await(Regex("30 100\\s+resized"), from = before)
        shell.type("exit 3")

        assertEquals(ShellEnd.Exited(3), withTimeout(20_000) { shell.ended.await() })
        assertEquals(null, shell.read())
        assertTrue(tunnel.state.value is ChannelState.Closed)
        assertNoJschThread()
        // Writes once it has ended are nothing.
        shell.write("echo late\n".encodeToByteArray())
        assertEquals(ShellEnd.Exited(3), shell.close())
    }

    @Test
    fun `the keys message carries a second client's key, which logs in, and the app closes the shell`() = test {
        val session = installed()
        val app = SshKey.generate()
        val other = SshKey.generate()
        val tunnel = SshTunnel.open(session, listOf(app.publicKeyLine(), other.publicKeyLine("laptop")))
        val shell = SshShell.open(tunnel, other, USER, columns = 80, rows = 24, scope = scope)
        val screen = Screen(shell, scope)
        shell.type("echo second-$((40 + 2))")
        screen.await(Regex("second-42"))

        assertEquals(ShellEnd.ByApp, shell.close())
        assertEquals(ChannelState.Closed(ChannelEnd.ByApp), tunnel.state.value)
        assertNoJschThread()
        // The link goes on: another shell opens on it.
        val again = SshShell.open(SshTunnel.open(session, listOf(app.publicKeyLine())), app, USER, columns = 80, rows = 24, scope = scope)
        assertEquals(ShellEnd.ByApp, again.close())
    }

    @Test
    fun `a host key other than the tunnel's is refused, and so is a key the relay was not given, with nothing left`() = test {
        val session = installed()
        val key = SshKey.generate()

        val pinned = SshTunnel.open(session, listOf(key.publicKeyLine()))
        val wrong = SshKey.generate().publicKeyLine().substringBeforeLast(' ')
        val refused = runCatching {
            SshShell.open(pinned, listOf(wrong), key, USER, 80, 24, scope, SshShell.HANDSHAKE_TIMEOUT_MS)
        }.exceptionOrNull()
        assertEquals(ShellEnd.HostKeyRefused, (refused as SshShellException).end)
        assertTrue(pinned.state.value is ChannelState.Closed)
        assertNoJschThread()

        val other = SshTunnel.open(session, listOf(key.publicKeyLine()))
        val stranger = runCatching { SshShell.open(other, SshKey.generate(), USER, 80, 24, scope) }.exceptionOrNull()
        assertEquals(ShellEnd.LoginRefused, (stranger as SshShellException).end)
        assertTrue(other.state.value is ChannelState.Closed)
        assertNoJschThread()
    }

    @Test
    fun `the switch turned off in mid-session ends the shell as cut, with nothing left`() = test {
        val session = installed()
        val key = SshKey.generate()
        val tunnel = SshTunnel.open(session, listOf(key.publicKeyLine()))
        val shell = SshShell.open(tunnel, key, USER, columns = 80, rows = 24, scope = scope)
        val screen = Screen(shell, scope)
        shell.type("echo ready")
        screen.await(Regex("ready"))
        shell.type("sleep 600")
        delay(300)

        assertTrue(!session.client.channelSet(SshTunnel.PACKAGE, SshTunnel.CHANNEL, false).on)
        assertEquals(ShellEnd.TunnelCut(ChannelEnd.ByAgent(ChannelCloseReason.OFF)), withTimeout(20_000) { shell.ended.await() })
        assertNoJschThread()
        assertEquals(ChannelError.OFF, (runCatching { SshTunnel.open(session, listOf(key.publicKeyLine())) }.exceptionOrNull() as SshTunnelException)
            .failure.let { (it as TunnelFailure.Refused).code })
    }

    @Test
    fun `the link lost in mid-session ends the shell as cut, with the link's reason`() = test {
        val session = installed()
        val key = SshKey.generate()
        val shell = SshShell.open(SshTunnel.open(session, listOf(key.publicKeyLine())), key, USER, columns = 80, rows = 24, scope = scope)
        val screen = Screen(shell, scope)
        shell.type("echo ready")
        screen.await(Regex("ready"))

        session.close("The test dropped the link")
        assertEquals(ShellEnd.TunnelCut(ChannelEnd.LinkLost("The test dropped the link")), withTimeout(20_000) { shell.ended.await() })
        assertNoJschThread()
    }

    @Test
    fun `the terminal opens over the app's link, shows a command's output, and is told the switch went off`() = test {
        // The test's own link, which switched SSH on, took the app's place: the agent keeps one link a phone. So the
        // terminal connects first, as the user's connect does, and that link takes the test's place in turn.
        installed()
        val key = SshKey.generate()
        val app = object : SshIdentity {
            override suspend fun key() = key
        }
        val kept = AtomicInteger()
        val terminal = TerminalRepository(JobTerminalLink(jobs), SshShellOpener(app, USER), { kept.incrementAndGet() }, scope)
        terminal.open("bench", 80, 24)
        assertEquals(1, kept.get())
        assertEquals(TerminalState.Open("bench"), withTimeoutOrNull(30_000) { terminal.state.first { !it.active || it is TerminalState.Open } })

        // Sent before the shell's prompt may have come: the pty echoes the line at once, and the shell then writes its
        // prompt and the command's output on one line ("$ term-42"), as a terminal shows typing ahead.
        terminal.send("echo term-$((6 * 7))\n".encodeToByteArray())
        val shown = withTimeoutOrNull(20_000) { terminal.lines.first { snapshot -> snapshot.lines.any { it.text.endsWith("term-42") } } }
        assertTrue("the terminal shows:\n${terminal.text()}", shown != null)

        // Switched off over the app's link, the one there is.
        val off = jobs.usingSession("bench") { it.client.channelSet(SshTunnel.PACKAGE, SshTunnel.CHANNEL, false) }
        assertTrue(!off.getOrThrow().on)
        assertEquals(TerminalState.Ended("bench", TerminalEnd.SwitchedOff), withTimeoutOrNull(20_000) { terminal.state.first { !it.active } })
        val marked = withTimeoutOrNull(5_000) { terminal.lines.first { it.lines.lastOrNull()?.mark == true } }
        assertEquals(TerminalLine(TerminalText.ended(TerminalEnd.SwitchedOff), mark = true), marked?.lines?.last())
        assertNoJschThread()

        // The link stays, and the next open is told that the switch is off.
        assertTrue(jobs.connection.value is ConnectionState.Connected)
        terminal.open("bench", 80, 24)
        assertEquals(TerminalState.Off("bench"), withTimeoutOrNull(20_000) { terminal.state.first { !it.active } })
    }

    @Test
    fun `the port lets an outside SSH client in with a saved client key, shows the head unit's host key, and runs a command`() = test {
        installed()
        // The test's own link took the app's place: the app connects again, as the user does.
        assertTrue(jobs.connect("bench").isSuccess)
        val laptop = SshKey.generate()
        val clients = InMemoryAuthorizedClients()
        assertTrue(clients.add("Laptop", laptop.publicKeyLine("laptop")) is AddClientResult.Added)
        val first = ServerSocket(0, 1, InetAddress.getByName("127.0.0.1")).use { it.localPort }
        val port = SshPort(JobPortLink(jobs), SshChannel, clients, {}, scope, firstPort = first, lastPort = first + 9)
        assertTrue(port.switchOn("bench"))
        val on = withTimeoutOrNull(60_000) { port.state.first { it !is SshPortState.Checking } }
        assertTrue("$on", on is SshPortState.On)
        on as SshPortState.On

        // JSch as the outside client, over a plain socket to the port, as an SSH app on the phone connects.
        val output = withContext(Dispatchers.IO) {
            val jsch = JSch()
            jsch.addIdentity(OutsideKey(laptop), null)
            val session = jsch.getSession(USER, "127.0.0.1", on.port)
            session.setConfig("StrictHostKeyChecking", "no")
            session.setConfig("PreferredAuthentications", "publickey")
            session.connect(30_000)
            try {
                // The host key the client is shown is the one the port's state gives.
                assertEquals(on.hostKey, SshKeyLines.fingerprint(Base64.getDecoder().decode(session.hostKey.key)))
                val exec = session.openChannel("exec") as ChannelExec
                exec.setCommand("echo port-$((6 * 7))")
                val stream = exec.inputStream
                exec.connect(30_000)
                stream.readBytes().decodeToString().also { exec.disconnect() }
            } finally {
                session.disconnect()
            }
        }
        assertEquals("port-42\n", output)
        port.switchOff()
        assertEquals(SshPortState.Off("bench"), withTimeoutOrNull(20_000) { port.state.first { it is SshPortState.Off } })
    }

    /** A key as JSch's [com.jcraft.jsch.Identity], signing with Bouncy Castle: the outside client's. */
    private class OutsideKey(private val key: SshKey) : com.jcraft.jsch.Identity {
        override fun setPassphrase(passphrase: ByteArray?): Boolean = true
        override fun getPublicKeyBlob(): ByteArray = key.blob
        override fun getSignature(data: ByteArray): ByteArray = key.sshSignature(data)
        override fun getSignature(data: ByteArray, alg: String?): ByteArray = key.sshSignature(data)
        override fun getAlgName(): String = SshKeyLines.ED25519
        override fun getName(): String = "laptop"
        override fun isEncrypted(): Boolean = false
        override fun clear() = Unit
    }

    private companion object {
        /** The user the bench's server logs in: the agent's own (`tools/bench.py`). A head unit's is root. */
        const val USER = "bench"
    }
}

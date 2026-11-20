// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.MemoryLink
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.ChannelCloseReason
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.withTestTimeout
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.Base64
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The SSH port for other apps against stand-ins for the link and the head unit's SSH channel ([FakeChannel], whose
 * tunnels send back what they are sent), with real sockets on 127.0.0.1. The port through the real agent and SSH
 * package, with JSch as the client, is in [SshRealAgentTest].
 */
class SshPortTest {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val sessionScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val session = AgentSession(MemoryLink.pair().first, sessionScope)
    private val sockets = CopyOnWriteArrayList<Socket>()
    private val held = CopyOnWriteArrayList<ServerSocket>()

    @After
    fun tearDown() {
        scope.cancel()
        sockets.forEach { runCatching { it.close() } }
        held.forEach { runCatching { it.close() } }
        session.close("the test is over")
        sessionScope.cancel()
    }

    private fun connected(headUnitId: String, sshVersion: String? = "1.0.0", channels: Boolean = true) = ConnectionState.Connected(
        headUnitId,
        TcpEndpoint("192.0.2.1", 7144),
        AgentInfo("1.7.0", "000102030405060708090a0b0c0d0e0f", 1),
        listOfNotNull(sshVersion?.let { InstalledPackage(SshTunnel.PACKAGE, "SSH access", it) }),
        capabilities = if (channels) listOf(ProtocolConstants.CAPABILITY_CHANNELS) else emptyList(),
    )

    private inner class FakeLink : PortLink {
        override val connection = MutableStateFlow<ConnectionState>(connected(CAR))
        override val jobOn = MutableStateFlow<String?>(null)
        val uses = AtomicInteger()

        override suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T> {
            if ((connection.value as? ConnectionState.Connected)?.headUnitId != headUnitId) return Result.failure(IOException("Not connected to a head unit"))
            uses.incrementAndGet()
            return try {
                Result.success(block(session))
            } catch (e: IOException) {
                Result.failure(e)
            } finally {
                uses.decrementAndGet()
            }
        }
    }

    /** A tunnel that sends back what it is sent, as an SSH server would answer, to the byte. */
    private class EchoTunnel(override val hostKeys: List<String>) : PortTunnel {
        private val queue = LinkedBlockingQueue<ByteArray>()
        override val state = MutableStateFlow<ChannelState>(ChannelState.Open)

        @Volatile
        var closed = false

        override val input: InputStream = object : InputStream() {
            private var piece = ByteArray(0)
            private var at = 0

            override fun read(): Int = throw UnsupportedOperationException()

            override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
                if (at >= piece.size) {
                    piece = queue.take()
                    at = 0
                    if (piece.isEmpty()) {
                        queue.put(piece)
                        return -1
                    }
                }
                val n = minOf(length, piece.size - at)
                piece.copyInto(buffer, offset, at, at + n)
                at += n
                return n
            }
        }

        override val output: OutputStream = object : OutputStream() {
            override fun write(byte: Int) = write(byteArrayOf(byte.toByte()), 0, 1)

            override fun write(bytes: ByteArray, offset: Int, length: Int) {
                if (closed) throw IOException("closed")
                queue.put(bytes.copyOfRange(offset, offset + length))
            }
        }

        override fun close() {
            if (closed) return
            closed = true
            queue.put(ByteArray(0))
            if (state.value !is ChannelState.Closed) state.value = ChannelState.Closed(ChannelEnd.ByApp)
        }

        /** The agent ends the channel, for [reason]. */
        fun endByAgent(reason: String) {
            state.value = ChannelState.Closed(ChannelEnd.ByAgent(reason))
            closed = true
            queue.put(ByteArray(0))
        }
    }

    private class FakeChannel : PortChannel {
        val tunnels = CopyOnWriteArrayList<EchoTunnel>()
        val keysSent = CopyOnWriteArrayList<List<String>>()

        @Volatile
        var failure: Exception? = null

        @Volatile
        var reading = SwitchReading.ON

        override suspend fun open(session: AgentSession, keys: List<String>): PortTunnel {
            keysSent += keys
            failure?.let { throw it }
            return EchoTunnel(listOf(HOST_KEY)).also { tunnels += it }
        }

        override suspend fun switch(session: AgentSession): SwitchReading = reading
    }

    private inner class Rig(
        pollMs: Long = 60_000,
        firstPort: Int = freePort(),
        lastPort: Int = firstPort + 9,
        blocked: String? = null,
        sockets: PortSockets = PlainPortSockets,
        addresses: () -> List<PortAddress> = { listOf(PortAddress(PortAddress.Kind.WIFI, "192.0.2.23")) },
    ) {
        val link = FakeLink()
        val channel = FakeChannel()
        val clients = InMemoryAuthorizedClients()
        val kept = AtomicInteger()
        val now = AtomicLong(1_000)
        val first = firstPort
        val port = SshPort(
            link, channel, clients, { kept.incrementAndGet() }, scope,
            clock = { now.get() },
            sockets = sockets,
            networkAddresses = addresses,
            firstPort = firstPort,
            lastPort = lastPort,
            idleMs = IDLE,
            idleCheckMs = 20,
            pollMs = pollMs,
            otherDevicesBlocked = { blocked },
        )

        suspend fun key(name: String = "Laptop", line: String = LAPTOP) = assertTrue(clients.add(name, line) is AddClientResult.Added)

        suspend fun <T : SshPortState> await(what: String, predicate: (SshPortState) -> Boolean): T {
            @Suppress("UNCHECKED_CAST")
            return (withTimeoutOrNull(10_000) { port.state.first(predicate) } ?: throw AssertionError("never came: $what; last was ${port.state.value}")) as T
        }

        suspend fun on(): SshPortState.On {
            if (clients.clients.first().isEmpty()) key()
            assertTrue(port.switchOn(CAR))
            return await("on") { it is SshPortState.On }
        }

        suspend fun off(): SshPortState.Off = await("off") { it is SshPortState.Off }

        suspend fun connections(n: Int): SshPortState.On = await("$n connections") { it is SshPortState.On && it.connections == n }

        fun client(on: SshPortState.On): Socket = Socket().also {
            it.soTimeout = 5_000
            it.connect(InetSocketAddress(InetAddress.getByName("127.0.0.1"), on.port), 2_000)
            sockets += it
        }
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTestTimeout(60_000, block) }

    /** Writes [text] and reads it back, as the tunnel sends it back. */
    private fun echo(socket: Socket, text: String): String {
        socket.getOutputStream().write(text.encodeToByteArray())
        val buffer = ByteArray(text.length)
        var got = 0
        while (got < buffer.size) {
            val n = socket.getInputStream().read(buffer, got, buffer.size - got)
            if (n < 0) break
            got += n
        }
        return buffer.copyOf(got).decodeToString()
    }

    /** Whether the far side has closed [socket]: a read gives its end. */
    private fun closedByFarSide(socket: Socket): Boolean = try {
        socket.getInputStream().read() < 0
    } catch (e: IOException) {
        true
    }

    // ---- On, and the bytes ----

    @Test
    fun `switching on checks the way, listens on 127_0_0_1, holds the link, and passes the bytes both ways untouched`() = test {
        val rig = Rig()
        rig.key()
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(1, rig.kept.get())
        val on = rig.await<SshPortState.On>("on") { it is SshPortState.On }
        assertEquals(rig.first, on.port)
        assertFalse(on.otherDevices)
        assertEquals(0, on.connections)
        assertEquals(SshKeyLines.fingerprint(SshKeyLines.blob(HOST_KEY)!!), on.hostKey)
        assertEquals(listOf(PortAddress(PortAddress.Kind.THIS_PHONE, "127.0.0.1")), on.addresses)
        assertTrue(on.active)
        // The check's tunnel is closed, and the link is held.
        assertTrue(rig.channel.tunnels.single().closed)
        assertEquals(1, rig.link.uses.get())

        val client = rig.client(on)
        assertEquals("SSH-2.0-test\r\n\u0000\u007f", echo(client, "SSH-2.0-test\r\n\u0000\u007f"))
        assertEquals(1, rig.connections(1).connections)
        // The client's keys, and never the app's own.
        assertEquals(listOf(listOf(LAPTOP), listOf(LAPTOP)), rig.channel.keysSent)

        // The client goes: its tunnel is closed.
        client.close()
        rig.connections(0)
        assertTrue(rig.channel.tunnels.last().closed)

        rig.port.switchOff()
        assertEquals(SshPortState.Off(CAR), rig.off())
        assertEquals(0, rig.link.uses.get())
        // The socket is gone.
        assertTrue(runCatching { Socket("127.0.0.1", on.port).close() }.isFailure)
    }

    @Test
    fun `a megabyte goes through and back`() = test {
        val rig = Rig()
        val client = rig.client(rig.on())
        val sent = ByteArray(1 shl 20) { (it * 31).toByte() }
        val reader = async(Dispatchers.IO) {
            val back = ByteArray(sent.size)
            var got = 0
            while (got < back.size) {
                val n = client.getInputStream().read(back, got, back.size - got)
                if (n < 0) break
                got += n
            }
            back.copyOf(got)
        }
        client.getOutputStream().write(sent)
        assertTrue(sent.contentEquals(reader.await()))
    }

    @Test
    fun `three connections at once, and a fourth is closed at once`() = test {
        val rig = Rig()
        val on = rig.on()
        val three = (1..3).map { rig.client(on).also { socket -> assertEquals("hi", echo(socket, "hi")) } }
        assertEquals(3, rig.connections(3).connections)
        val fourth = rig.client(on)
        assertTrue(closedByFarSide(fourth))
        assertEquals(3, rig.port.state.value.let { (it as SshPortState.On).connections })
        assertEquals("still", echo(three[0], "still"))

        // One goes, and another may come.
        three[2].close()
        rig.connections(2)
        assertEquals("again", echo(rig.client(on), "again"))
    }

    @Test
    fun `the port is the first free one from 2222 on, up to the last, and none free is a failure`() = test {
        val first = freePort()
        held += ServerSocket(first, 8, InetAddress.getByName("127.0.0.1"))
        val rig = Rig(firstPort = first, lastPort = first + 9)
        assertEquals(first + 1, rig.on().port)
        rig.port.switchOff()
        rig.off()

        held += ServerSocket(first + 1, 8, InetAddress.getByName("127.0.0.1"))
        val full = Rig(firstPort = first, lastPort = first + 1)
        full.key()
        assertTrue(full.port.switchOn(CAR))
        assertEquals(SshPortState.Off(CAR, failure = SshPort.NO_FREE_PORT), full.off())
        assertEquals(SshPort.FIRST_PORT, 2222)
        assertEquals(SshPort.LAST_PORT, 2231)
    }

    // ---- The keys ----

    @Test
    fun `each connection carries the keys as they are when it is made`() = test {
        val rig = Rig()
        val on = rig.on()
        rig.key("Phone app", PHONE)
        assertEquals("x", echo(rig.client(on), "x"))
        assertEquals(listOf(LAPTOP, PHONE), rig.channel.keysSent.last())
    }

    @Test
    fun `removing a key closes every connection, and the port goes off with the last key`() = test {
        val rig = Rig()
        rig.key()
        rig.key("Phone app", PHONE)
        val on = rig.on()
        val a = rig.client(on)
        val b = rig.client(on)
        assertEquals("a", echo(a, "a"))
        assertEquals("b", echo(b, "b"))
        rig.connections(2)

        rig.clients.remove(rig.clients.clients.first().first { it.name == "Phone app" }.fingerprint)
        assertTrue(closedByFarSide(a))
        assertTrue(closedByFarSide(b))
        val still = rig.connections(0)
        assertEquals(on.port, still.port)
        assertEquals("c", echo(rig.client(still), "c"))

        rig.clients.remove(rig.clients.clients.first().single().fingerprint)
        assertEquals(SshPortState.Off(CAR), rig.off())
    }

    // ---- Refused ----

    @Test
    fun `switching on is refused with no key, no link, an agent without channels, no package, and SSH access off`() = test {
        val rig = Rig()
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(SshPortState.Off(CAR, failure = SshPort.NO_KEYS), rig.off())

        rig.key()
        rig.link.connection.value = ConnectionState.Disconnected
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(SshPortState.Off(CAR, failure = SshPort.NOT_CONNECTED), rig.off())

        rig.link.connection.value = connected(CAR, channels = false)
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(SshPortState.Off(CAR, failure = SshPort.TOO_OLD), rig.off())

        rig.link.connection.value = connected(CAR, sshVersion = null)
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(SshPortState.Off(CAR, failure = SshPort.NO_PACKAGE), rig.off())

        rig.link.connection.value = connected(CAR)
        rig.channel.failure = SshTunnelException(TunnelFailure.Refused("off", "off"))
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(SshPortState.Off(CAR, failure = "SSH access is switched off on the head unit."), rig.off())
        assertEquals(0, rig.link.uses.get())
    }

    @Test
    fun `each failure of the check is a line, and the port stays off`() = test {
        val rig = Rig()
        rig.key()
        for ((failure, line) in listOf(
            TunnelFailure.Refused("busy", "busy") to "The head unit is busy with a job. Try again once it has finished.",
            TunnelFailure.Refused("not-found", "not-found") to SshPort.NO_PACKAGE,
            TunnelFailure.Refused("too-many", "too-many") to "The link to the head unit has as many connections open as it can take.",
            TunnelFailure.Relay("too-many") to "The head unit's SSH server has as many connections as it takes.",
            TunnelFailure.Relay("failed") to "The head unit's SSH server couldn't be started.",
            TunnelFailure.NoAnswer(toKeys = true) to "The head unit didn't answer in time.",
            TunnelFailure.Closed(ChannelEnd.LinkLost("The link was lost")) to "The link was lost",
        )) {
            rig.channel.failure = SshTunnelException(failure)
            assertTrue(rig.port.switchOn(CAR))
            assertEquals("$failure", SshPortState.Off(CAR, failure = line), rig.off())
        }
    }

    @Test
    fun `switching on for another head unit while on is refused, and changes nothing`() = test {
        val rig = Rig()
        val on = rig.on()
        assertFalse(rig.port.switchOn("other"))
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(on, rig.port.state.value)
    }

    // ---- Off by itself ----

    @Test
    fun `after the idle time with no connection it goes off, the clock starting again when the last connection closes`() = test {
        val rig = Rig()
        val on = rig.on()
        rig.now.addAndGet(IDLE - 10)
        val client = rig.client(on)
        assertEquals("x", echo(client, "x"))
        // A connection open: no idle time counts.
        rig.now.addAndGet(IDLE * 3)
        delay(200)
        assertTrue(rig.port.state.value is SshPortState.On)

        client.close()
        rig.connections(0)
        rig.now.addAndGet(IDLE - 10)
        delay(200)
        assertTrue(rig.port.state.value is SshPortState.On)
        rig.now.addAndGet(20)
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.IDLE), rig.off())
    }

    @Test
    fun `the link lost turns it off, and nothing connects again`() = test {
        val rig = Rig()
        val client = rig.client(rig.on())
        assertEquals("x", echo(client, "x"))
        rig.link.connection.value = ConnectionState.Failed(CAR, "lost")
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.LINK_LOST), rig.off())
        assertTrue(closedByFarSide(client))
        assertEquals(0, rig.link.uses.get())
    }

    @Test
    fun `the session ending turns it off as a lost link`() = test {
        val rig = Rig()
        rig.on()
        session.close("dropped")
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.LINK_LOST), rig.off())
    }

    @Test
    fun `SSH access switched off turns it off, seen by a look, by a connection's end, by a refused open, or told by the app`() = test {
        val rig = Rig(pollMs = 50)
        rig.on()
        rig.channel.reading = SwitchReading.OFF
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.SWITCHED_OFF), rig.off())

        rig.channel.reading = SwitchReading.ON
        val again = Rig()
        val on = again.on()
        val client = again.client(on)
        assertEquals("x", echo(client, "x"))
        again.channel.tunnels.last().endByAgent(ChannelCloseReason.OFF)
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.SWITCHED_OFF), again.off())

        val third = Rig()
        val open = third.on()
        third.channel.failure = SshTunnelException(TunnelFailure.Refused("off", "off"))
        third.client(open)
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.SWITCHED_OFF), third.off())

        val fourth = Rig()
        fourth.on()
        fourth.port.noteSwitch("other", false)
        fourth.port.noteSwitch(CAR, true)
        delay(100)
        assertTrue(fourth.port.state.value is SshPortState.On)
        fourth.port.noteSwitch(CAR, false)
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.SWITCHED_OFF), fourth.off())
    }

    @Test
    fun `the package removed or upgraded turns it off`() = test {
        val rig = Rig()
        rig.on()
        rig.link.connection.value = connected(CAR, sshVersion = "1.0.1")
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.PACKAGE_CHANGED), rig.off())

        rig.link.connection.value = connected(CAR)
        rig.on()
        rig.link.connection.value = connected(CAR, sshVersion = null)
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.PACKAGE_CHANGED), rig.off())

        rig.link.connection.value = connected(CAR)
        val on = rig.on()
        assertEquals("x", echo(rig.client(on), "x"))
        rig.channel.tunnels.last().endByAgent(ChannelCloseReason.PACKAGE_CHANGED)
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.PACKAGE_CHANGED), rig.off())

        val polled = Rig(pollMs = 50)
        polled.on()
        polled.channel.reading = SwitchReading.GONE
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.PACKAGE_CHANGED), polled.off())
    }

    // ---- A job ----

    @Test
    fun `a job on the head unit shows it paused, a held channel too, and a connection the agent refuses is closed`() = test {
        val rig = Rig()
        val on = rig.on()
        val client = rig.client(on)
        assertEquals("x", echo(client, "x"))
        rig.link.jobOn.value = CAR
        rig.await<SshPortState.On>("paused") { it is SshPortState.On && it.paused }
        rig.link.jobOn.value = "other"
        rig.await<SshPortState.On>("not paused") { it is SshPortState.On && !it.paused }
        rig.channel.tunnels.last().state.value = ChannelState.Paused
        rig.await<SshPortState.On>("held") { it is SshPortState.On && it.paused }
        rig.channel.tunnels.last().state.value = ChannelState.Open
        rig.await<SshPortState.On>("going on") { it is SshPortState.On && !it.paused }

        rig.channel.failure = SshTunnelException(TunnelFailure.Refused("busy", "busy"))
        val refused = rig.client(on)
        assertTrue(closedByFarSide(refused))
        assertTrue(rig.port.state.value is SshPortState.On)
        assertEquals("y", echo(client, "y"))
    }

    // ---- Other devices ----

    @Test
    fun `other devices are let in on every address, the open connections stay, and it goes back to off with the port`() = test {
        val rig = Rig()
        val on = rig.on()
        val before = rig.client(on)
        assertEquals("a", echo(before, "a"))

        rig.port.allowOtherDevices(true)
        val wide = rig.await<SshPortState.On>("other devices") { it is SshPortState.On && it.otherDevices }
        assertEquals(on.port, wide.port)
        assertEquals(
            listOf(PortAddress(PortAddress.Kind.THIS_PHONE, "127.0.0.1"), PortAddress(PortAddress.Kind.WIFI, "192.0.2.23")),
            wide.addresses,
        )
        assertNull(wide.otherDevicesFailure)
        assertEquals("b", echo(before, "b"))
        assertEquals("c", echo(rig.client(wide), "c"))

        rig.port.allowOtherDevices(false)
        val narrow = rig.await<SshPortState.On>("this phone only") { it is SshPortState.On && !it.otherDevices }
        assertEquals(1, narrow.addresses.size)
        assertEquals("d", echo(before, "d"))
        assertEquals("e", echo(rig.client(narrow), "e"))

        rig.port.allowOtherDevices(true)
        rig.await<SshPortState.On>("other devices") { it is SshPortState.On && it.otherDevices }
        rig.port.switchOff()
        rig.off()
        assertFalse(rig.on().otherDevices)
    }

    @Test
    fun `other devices can be allowed only while the port is on`() = test {
        val rig = Rig()
        rig.key()
        rig.port.allowOtherDevices(true)
        delay(100)
        assertFalse(rig.on().otherDevices)
    }

    @Test
    fun `where Android keeps other devices out, they stay out with a line, and the port goes on as it was`() = test {
        val rig = Rig(blocked = LOCAL_NETWORK_BLOCKED)
        val on = rig.on()
        rig.port.allowOtherDevices(true)
        val told = rig.await<SshPortState.On>("the line") { it is SshPortState.On && it.otherDevicesFailure != null }
        assertEquals(LOCAL_NETWORK_BLOCKED, told.otherDevicesFailure)
        assertFalse(told.otherDevices)
        assertEquals("x", echo(rig.client(on), "x"))

        assertEquals(LOCAL_NETWORK_BLOCKED, localNetworkBlocked(sdk = 37, targetSdk = 37, granted = false))
        assertNull(localNetworkBlocked(sdk = 37, targetSdk = 37, granted = true))
        assertNull(localNetworkBlocked(sdk = 36, targetSdk = 37, granted = false))
        assertNull(localNetworkBlocked(sdk = 37, targetSdk = 36, granted = false))
        assertEquals("Android keeps other devices from reaching this app until the Nearby devices permission is allowed.", LOCAL_NETWORK_BLOCKED)
    }

    @Test
    fun `the permission is asked for only on Android 17 and later, for an app that targets it, and while it is not granted`() {
        assertTrue(localNetworkNeeded(sdk = 37, targetSdk = 37, granted = false))
        assertTrue(localNetworkNeeded(sdk = 38, targetSdk = 37, granted = false))
        assertFalse(localNetworkNeeded(sdk = 37, targetSdk = 37, granted = true))
        assertFalse(localNetworkNeeded(sdk = 36, targetSdk = 37, granted = false))
        assertFalse(localNetworkNeeded(sdk = 37, targetSdk = 36, granted = false))
    }

    @Test
    fun `with other devices allowed the phone's addresses are read again at each look, and given out where they changed`() = test {
        val addresses = java.util.concurrent.atomic.AtomicReference(listOf(PortAddress(PortAddress.Kind.WIFI, "192.0.2.23")))
        val rig = Rig(pollMs = 50, addresses = { addresses.get() })
        rig.on()
        rig.port.allowOtherDevices(true)
        rig.await<SshPortState.On>("other devices") { it is SshPortState.On && it.otherDevices }
        // A hotspot switched on later.
        addresses.set(listOf(PortAddress(PortAddress.Kind.HOTSPOT, "192.0.2.1"), PortAddress(PortAddress.Kind.WIFI, "192.0.2.23")))
        val later = rig.await<SshPortState.On>("the hotspot") { it is SshPortState.On && it.addresses.size == 3 }
        assertEquals(PortAddress(PortAddress.Kind.HOTSPOT, "192.0.2.1"), later.addresses[1])
    }

    @Test
    fun `a port lost while other devices are let in goes off with a line`() = test {
        val calls = AtomicInteger()
        // The first socket is the real one; every later one cannot be had.
        val failing = PortSockets { allDevices, port ->
            if (calls.getAndIncrement() == 0) PlainPortSockets.listen(allDevices, port) else throw java.net.BindException("Address already in use")
        }
        val rig = Rig(sockets = failing)
        rig.on()
        rig.port.allowOtherDevices(true)
        val off = rig.off()
        assertEquals(CAR, off.headUnitId)
        assertNull(off.reason)
        assertEquals("The port couldn't be opened again on this phone: Address already in use", off.failure)
    }

    @Test
    fun `an interface's name tells its kind`() {
        assertEquals(PortAddress.Kind.WIFI, interfaceKind("wlan0"))
        assertEquals(PortAddress.Kind.HOTSPOT, interfaceKind("wlan1"))
        assertEquals(PortAddress.Kind.HOTSPOT, interfaceKind("ap0"))
        assertEquals(PortAddress.Kind.HOTSPOT, interfaceKind("swlan0"))
        assertEquals(PortAddress.Kind.NETWORK, interfaceKind("rndis0"))
        assertEquals(PortAddress.Kind.NETWORK, interfaceKind("eth0"))
    }

    @Test
    fun `the host key's fingerprint is the ed25519 one's`() {
        val ecdsa = "ecdsa-sha2-nistp256 " + Base64.getEncoder().encodeToString(
            SshKeyLines.sshStrings("ecdsa-sha2-nistp256".encodeToByteArray(), "nistp256".encodeToByteArray(), ByteArray(65).also { it[0] = 4 }),
        )
        assertEquals(SshKeyLines.fingerprint(SshKeyLines.blob(HOST_KEY)!!), hostKeyFingerprint(listOf(ecdsa, HOST_KEY)))
        assertNull(hostKeyFingerprint(emptyList()))
    }

    private companion object {
        const val CAR = "car"
        const val IDLE = 30 * 60_000L
        const val HOST_KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4f"
        const val LAPTOP = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g laptop"
        val PHONE = "ssh-ed25519 " + Base64.getEncoder().encodeToString(
            SshKeyLines.sshStrings("ssh-ed25519".encodeToByteArray(), ByteArray(32) { (it + 40).toByte() }),
        )

        /** A port, and the nine after it, that are likely free: one the system has just given and taken back. */
        fun freePort(): Int {
            while (true) {
                val port = ServerSocket(0, 1, InetAddress.getByName("127.0.0.1")).use { it.localPort }
                if (port < 65_000) return port
            }
        }
    }
}

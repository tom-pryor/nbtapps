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
import java.net.BindException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.Base64
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalForInheritanceCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flow
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
    // A link that carries nothing and stays open until the test is over, however long it takes: a lost link is only
    // ever one a test brings about.
    private val session = AgentSession(MemoryLink.pair(readLimitSeconds = null).first, sessionScope)
    private val sockets = CopyOnWriteArrayList<Socket>()

    @After
    fun tearDown() {
        scope.cancel()
        sockets.forEach { runCatching { it.close() } }
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
        private val current = MutableStateFlow<ChannelState>(ChannelState.Open)

        /** What a coroutine that collects [state] is given: [current], except what came while [unseen]. */
        private val given = MutableStateFlow<ChannelState>(ChannelState.Open)

        /** While set, a coroutine that collects [state] is given nothing new, as one not yet run would not be. */
        @Volatile
        var unseen = false

        /** Its [value] is always the state as it is, as a [com.nbtapps.store.data.channels.ChannelLink]'s is. */
        @OptIn(ExperimentalForInheritanceCoroutinesApi::class)
        override val state: StateFlow<ChannelState> = object : StateFlow<ChannelState> by given {
            override val value: ChannelState get() = current.value
        }

        /** The channel is now in [now]. */
        fun set(now: ChannelState) {
            current.value = now
            if (!unseen) given.value = now
        }

        @Volatile
        var closed = false

        /** Run as the port takes [input], before it has it. */
        @Volatile
        var onInput: () -> Unit = {}

        override val input: InputStream
            get() {
                onInput()
                return echoes
            }

        private val echoes: InputStream = object : InputStream() {
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
            if (state.value !is ChannelState.Closed) set(ChannelState.Closed(ChannelEnd.ByApp))
        }

        /**
         * The agent ends the channel, for [reason]: its state says so first, and then, where [streamEnds], what is read
         * from it ends, as a [com.nbtapps.store.data.channels.ChannelLink]'s does. Without, the stream stays open until
         * the port closes the tunnel.
         */
        fun endByAgent(reason: String, streamEnds: Boolean = true) {
            set(ChannelState.Closed(ChannelEnd.ByAgent(reason)))
            if (!streamEnds) return
            closed = true
            queue.put(ByteArray(0))
        }
    }

    /**
     * The saved keys, with the port's watch of them in the test's hands: the second to collect them, after the read as
     * the port goes on, so long as no client connects before [watchBegan].
     */
    private class HeldClients(private val keys: InMemoryAuthorizedClients = InMemoryAuthorizedClients()) : AuthorizedClients by keys {
        private val collections = AtomicInteger()

        /** The watch has begun, and has seen nothing. */
        val watchBegan = CompletableDeferred<Unit>()

        /** The watch may see the keys. */
        val letWatchSee = CompletableDeferred<Unit>()

        /** The watch has seen the keys once, and is done with that. */
        val watchSaw = CompletableDeferred<Unit>()

        override val clients: Flow<List<ClientKey>> = flow {
            if (collections.incrementAndGet() != WATCH) {
                emitAll(keys.clients)
            } else {
                watchBegan.complete(Unit)
                letWatchSee.await()
                keys.clients.collect {
                    emit(it)
                    watchSaw.complete(Unit)
                }
            }
        }

        private companion object {
            const val WATCH = 2
        }
    }

    private class FakeChannel : PortChannel {
        val tunnels = CopyOnWriteArrayList<EchoTunnel>()
        val keysSent = CopyOnWriteArrayList<List<String>>()

        @Volatile
        var failure: Exception? = null

        @Volatile
        var reading = SwitchReading.ON

        /** What a test sets on a tunnel before the port has it. */
        @Volatile
        var prepare: (EchoTunnel) -> Unit = {}

        /** An open waits for this, once it has told [opening]. */
        @Volatile
        var letOpen: CompletableDeferred<Unit>? = null
        val opening = CompletableDeferred<Unit>()

        override suspend fun open(session: AgentSession, keys: List<String>): PortTunnel {
            keysSent += keys
            failure?.let { throw it }
            letOpen?.let {
                opening.complete(Unit)
                it.await()
            }
            return EchoTunnel(listOf(HOST_KEY)).also {
                prepare(it)
                tunnels += it
            }
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
        val clients: AuthorizedClients = InMemoryAuthorizedClients(),
    ) {
        val link = FakeLink()
        val channel = FakeChannel()
        val kept = AtomicInteger()
        val now = AtomicLong(1_000)
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
            return switchedOn()
        }

        /** [on] where a key is saved: the keys are read by the port alone. */
        suspend fun switchedOn(): SshPortState.On {
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

    /** Whether the far side has closed [socket]: a read gives its end, or fails. One that only waits in vain is open. */
    private fun closedByFarSide(socket: Socket): Boolean = try {
        socket.getInputStream().read() < 0
    } catch (e: SocketTimeoutException) {
        false
    } catch (e: IOException) {
        true
    }

    // ---- On, and the bytes ----

    @Test
    fun `switching on checks the way, listens on 127_0_0_1, holds the link, and passes the bytes both ways untouched`() = test {
        val listened = CopyOnWriteArrayList<ServerSocket>()
        val rig = Rig(sockets = { allDevices, port -> PlainPortSockets.listen(allDevices, port).also { listened += it } })
        rig.key()
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(1, rig.kept.get())
        val on = rig.await<SshPortState.On>("on") { it is SshPortState.On }
        assertEquals(listened.single().localPort, on.port)
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
        assertTrue(listened.single().isClosed)
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
        // Which ports are taken is the test's say, not the machine's: the sockets themselves are on whatever is free.
        val taken = CopyOnWriteArrayList(listOf(SshPort.FIRST_PORT))
        val some = PortSockets { _, port ->
            if (port in taken) throw BindException("Address already in use")
            ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"))
        }
        val rig = Rig(firstPort = SshPort.FIRST_PORT, lastPort = SshPort.LAST_PORT, sockets = some)
        assertEquals(2223, rig.on().port)
        rig.port.switchOff()
        rig.off()

        taken += 2223..2230
        assertEquals(2231, rig.on().port)
        rig.port.switchOff()
        rig.off()

        taken += 2231
        assertTrue(rig.port.switchOn(CAR))
        assertEquals(SshPortState.Off(CAR, failure = SshPort.NO_FREE_PORT), rig.off())
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

    /** Two keys saved and two connections open, and the test's hand on the port's watch of the keys. */
    private suspend fun twoConnections(clients: HeldClients, whenOn: suspend () -> Unit): Triple<Rig, Socket, Socket> {
        val rig = Rig(clients = clients)
        rig.key()
        rig.key("Phone app", PHONE)
        val on = rig.switchedOn()
        whenOn()
        val a = rig.client(on)
        val b = rig.client(on)
        assertEquals("a", echo(a, "a"))
        assertEquals("b", echo(b, "b"))
        rig.connections(2)
        return Triple(rig, a, b)
    }

    /** The user's own switching off: a fault of a connection before it would show as the failure it goes off with. */
    private suspend fun Rig.offByTheUser() {
        port.switchOff()
        assertEquals(SshPortState.Off(CAR), off())
    }

    private suspend fun Rig.removePhone() = clients.remove(SshKeyLines.fingerprint(SshKeyLines.blob(PHONE)!!))

    @Test
    fun `a key removed before the port first looks at the keys closes every connection`() = test {
        val clients = HeldClients()
        val (rig, a, b) = twoConnections(clients) { clients.watchBegan.await() }
        rig.removePhone()
        clients.letWatchSee.complete(Unit)
        assertTrue(closedByFarSide(a))
        assertTrue(closedByFarSide(b))
        rig.connections(0)
    }

    @Test
    fun `a key removed after the port first looked at the keys closes every connection`() = test {
        val clients = HeldClients()
        val (rig, a, b) = twoConnections(clients) {
            clients.watchBegan.await()
            clients.letWatchSee.complete(Unit)
            clients.watchSaw.await()
        }
        rig.removePhone()
        assertTrue(closedByFarSide(a))
        assertTrue(closedByFarSide(b))
        rig.connections(0)
    }

    @Test
    fun `a key removed while a connection's tunnel opens closes that connection, and the port stays on`() = test {
        val rig = Rig()
        rig.key()
        rig.key("Phone app", PHONE)
        val on = rig.on()
        val letOpen = CompletableDeferred<Unit>()
        rig.channel.letOpen = letOpen
        val client = rig.client(on)
        rig.channel.opening.await()
        rig.removePhone()
        assertTrue(closedByFarSide(client))
        letOpen.complete(Unit)
        rig.connections(0)
        assertTrue(rig.channel.tunnels.last().closed)
        rig.channel.letOpen = null
        assertEquals("c", echo(rig.client(on), "c"))
        assertEquals(listOf(LAPTOP), rig.channel.keysSent.last())
        rig.offByTheUser()
    }

    @Test
    fun `a key removed as a connection's bytes begin to pass closes that connection, and the port stays on`() = test {
        val rig = Rig()
        rig.key()
        rig.key("Phone app", PHONE)
        val on = rig.on()
        val taking = CompletableDeferred<Unit>()
        val go = CountDownLatch(1)
        rig.channel.prepare = {
            it.onInput = {
                taking.complete(Unit)
                go.await()
            }
        }
        val client = rig.client(on)
        taking.await()
        rig.removePhone()
        assertTrue(closedByFarSide(client))
        go.countDown()
        rig.connections(0)
        assertTrue(rig.channel.tunnels.last().closed)
        rig.channel.prepare = {}
        assertEquals("c", echo(rig.client(on), "c"))
        rig.offByTheUser()
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
    fun `SSH access switched off turns it off, seen by a look, by a refused open, or told by the app`() = test {
        val rig = Rig(pollMs = 50)
        rig.on()
        rig.channel.reading = SwitchReading.OFF
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.SWITCHED_OFF), rig.off())

        val third = Rig()
        val open = third.on()
        third.channel.failure = SshTunnelException(TunnelFailure.Refused("off", "off"))
        third.client(open)
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.SWITCHED_OFF), third.off())

        // Told of another head unit, or of SSH access switched on, it goes on: the reason it goes off with is the
        // user's own switching off after them, which has none.
        val fourth = Rig()
        fourth.on()
        fourth.port.noteSwitch("other", false)
        fourth.port.noteSwitch(CAR, true)
        fourth.port.switchOff()
        assertEquals(SshPortState.Off(CAR), fourth.off())

        val fifth = Rig()
        fifth.on()
        fifth.port.noteSwitch(CAR, false)
        assertEquals(SshPortState.Off(CAR, reason = PortOffReason.SWITCHED_OFF), fifth.off())
    }

    /** The ends of a connection's channel by the agent that turn the port off, with the reason each turns it off for. */
    private val agentEnds = listOf(ChannelCloseReason.OFF to PortOffReason.SWITCHED_OFF, ChannelCloseReason.PACKAGE_CHANGED to PortOffReason.PACKAGE_CHANGED)

    @Test
    fun `a connection's channel the agent ends turns it off for the agent's reason, where the port sees the end before the channel's stream ends`() = test {
        for ((why, reason) in agentEnds) {
            val rig = Rig()
            val client = rig.client(rig.on())
            assertEquals("x", echo(client, "x"))
            // The stream stays open: only the port's look at the channel's state can turn it off.
            rig.channel.tunnels.last().endByAgent(why, streamEnds = false)
            assertEquals(why, SshPortState.Off(CAR, reason = reason), rig.off())
            assertTrue(closedByFarSide(client))
        }
    }

    @Test
    fun `a connection's channel the agent ends turns it off for the agent's reason, where the channel's stream ends before the port sees the end`() = test {
        for ((why, reason) in agentEnds) {
            val rig = Rig()
            val client = rig.client(rig.on())
            assertEquals("x", echo(client, "x"))
            // The port's look at the channel's state is given nothing more: only what the state says once the stream
            // has ended can turn it off.
            val tunnel = rig.channel.tunnels.last()
            tunnel.unseen = true
            tunnel.endByAgent(why)
            assertEquals(why, SshPortState.Off(CAR, reason = reason), rig.off())
            assertTrue(closedByFarSide(client))
        }
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
        rig.channel.tunnels.last().set(ChannelState.Paused)
        rig.await<SshPortState.On>("held") { it is SshPortState.On && it.paused }
        rig.channel.tunnels.last().set(ChannelState.Open)
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
            if (calls.getAndIncrement() == 0) PlainPortSockets.listen(allDevices, port) else throw BindException("Address already in use")
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

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.withTestTimeout
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.currentTime
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * The serial connector against a stand-in for the phone's Bluetooth ([FakeRadio]). The tests of the times run on the
 * test's own clock; those of the phone's dial and the car's link racing each other run on real threads, as the
 * listener's accept blocks one, and wait for what they need to see without sleeping.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class BluetoothSerialLinkConnectorTest {

    private val car = SerialEndpoint("00:11:22:33:44:55", "BMW 12345")
    private val stranger = "66:77:88:99:AA:BB"

    /**
     * A connector on the test's own clock: a pause between tries takes no real time. Its dial's blocking calls run on
     * [io]; its listener blocks a thread of its own, which takes no time on the test's clock.
     */
    private fun TestScope.connector(
        radio: SerialRadio,
        io: CoroutineDispatcher = StandardTestDispatcher(testScheduler),
        times: SerialConnectTimes = SerialConnectTimes(),
        listenIo: CoroutineDispatcher = Dispatchers.IO,
    ) = BluetoothSerialLinkConnector(radio, times, now = { currentTime }, io = io, listener = SerialListener(radio, listenIo))

    private suspend fun failureOf(connector: LinkConnector, endpoint: Endpoint, dials: Boolean = false): String = try {
        connector.connect(endpoint, dials)
        fail("the connect went through")
        ""
    } catch (e: IOException) {
        e.message.orEmpty()
    }

    /** A test on real threads, for at most a minute. */
    private fun live(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTestTimeout(60_000, block) }

    private suspend fun until(what: String, condition: () -> Boolean) {
        withTimeoutOrNull(10_000) { while (!condition()) delay(5) } ?: throw AssertionError("never came: $what")
    }

    /**
     * A socket whose connect waits for [connectGate] and whose first write waits for [writeGate], each where given:
     * a link that comes up when the test says so.
     */
    private class GatedSocket(private val connectGate: CountDownLatch? = null, private val writeGate: CountDownLatch? = null) : SerialSocket {
        val closes = AtomicInteger()
        val writing = CountDownLatch(1)
        override val input = ByteArrayInputStream(ByteArray(0))
        override val output = object : ByteArrayOutputStream() {
            override fun write(bytes: ByteArray, offset: Int, length: Int) {
                writing.countDown()
                writeGate?.await(10, TimeUnit.SECONDS)
                super.write(bytes, offset, length)
            }
        }

        override fun connect() {
            connectGate?.await(10, TimeUnit.SECONDS)
        }

        override fun close() {
            closes.incrementAndGet()
        }
    }

    @Test
    fun `reaches Bluetooth serial endpoints only`() = runTest {
        val connector = connector(FakeRadio())
        assertTrue(connector.supports(car))
        assertFalse(connector.supports(TcpEndpoint("192.168.0.1", 7144)))
        assertTrue(failureOf(connector, TcpEndpoint("192.168.0.1", 7144)).isNotEmpty())
    }

    @Test
    fun `dials the agent's service on the endpoint's device, and the link is a serial one over that socket`() = runTest {
        val radio = FakeRadio()
        val link = connector(radio).connect(car)

        assertEquals(listOf("00:11:22:33:44:55" to UUID.fromString("58876f35-7f3f-4592-b65d-9912589a4f51")), radio.asked)
        val socket = radio.fakeSockets.single()
        assertEquals(Link.SERIAL, link.kind)
        assertSame(socket.input, link.input)
        assertSame(socket.output, link.output)
        assertEquals(0, socket.closes.get())
        assertEquals(0, currentTime)

        link.close()
        assertEquals(1, socket.closes.get())
    }

    @Test
    fun `the magic bytes go out with the connect, and the link says that they have`() = runTest {
        val radio = FakeRadio()
        val link = connector(radio).connect(car)

        // Nothing has been asked of the link yet: the agent sees how it opens without waiting for a first frame.
        assertArrayEquals(FrameCodec.MAGIC, radio.fakeSockets.single().written)
        assertTrue(link.magicSent)
        assertFalse(MemoryLink.pair(kind = Link.SERIAL).first.magicSent)
    }

    @Test
    fun `a link that comes up and cannot take the magic bytes is a try that failed`() = runTest {
        val radio = FakeRadio { attempt -> FakeSocket(writeFailure = if (attempt < 3) "Broken pipe" else null) }
        val link = connector(radio).connect(car)

        assertEquals(3, radio.sockets.size)
        assertEquals(listOf(1, 1, 0), radio.fakeSockets.map { it.closes.get() })
        assertSame(radio.sockets.last().output, link.output)
        assertArrayEquals(FrameCodec.MAGIC, radio.fakeSockets.last().written)
    }

    @Test
    fun `a session over the link says hello behind the connector's magic bytes, with none of its own`() = runBlocking {
        // The socket is one end of a link in memory; a stand-in agent reads the other end as the agent does: the
        // magic bytes once, then frames.
        val (phone, other) = MemoryLink.pair()
        val socket = object : SerialSocket {
            override fun connect() {}
            override val input = phone.input
            override val output = phone.output
            override fun close() = phone.close()
        }
        val radio = object : SerialRadio {
            override fun unavailable(address: String): String? = null
            override fun socket(address: String, uuid: UUID): SerialSocket = socket
        }
        val agent = FakeAgent(other, "000102030405060708090a0b0c0d0e0f")
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val session = AgentSession(BluetoothSerialLinkConnector(radio).connect(car), scope)
            val hello = withTimeoutOrNull(10_000) { session.client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9") }

            assertEquals("000102030405060708090a0b0c0d0e0f", hello?.headUnitId)
            assertEquals(1, agent.requestsOf("hello").size)
            session.close("The test is over")
        } finally {
            scope.cancel()
        }
    }

    @Test
    fun `Bluetooth that cannot be used fails at once with the reason, and nothing is dialled or listened on`() = runTest {
        val radio = FakeRadio()
        radio.unavailable = "Bluetooth is off"
        assertEquals("Bluetooth is off", failureOf(connector(radio), car))
        assertTrue(radio.sockets.isEmpty())
        assertTrue(radio.listens.isEmpty())
        assertEquals(0, currentTime)
    }

    @Test
    fun `a connect that fails is tried again on a new socket, and the failed ones are closed`() = runTest {
        // Another app holds the car's one serial link for the first three tries.
        val radio = FakeRadio { attempt -> FakeSocket(if (attempt < 4) "read failed, socket might closed or timeout" else null) }
        val link = connector(radio).connect(car)

        assertEquals(4, radio.sockets.size)
        assertEquals(listOf(1, 1, 1, 0), radio.fakeSockets.map { it.closes.get() })
        // Pauses of 0.7 s, 1.4 s and 2.8 s.
        assertEquals(4_900, currentTime)
        assertSame(radio.sockets.last().input, link.input)
    }

    @Test
    fun `the pause between tries starts at 700 ms and doubles up to 5 s`() = runTest {
        val starts = CopyOnWriteArrayList<Long>()
        val radio = FakeRadio {
            starts += currentTime
            FakeSocket("read failed, socket might closed or timeout, read ret: -1")
        }
        failureOf(connector(radio), car)

        assertEquals(listOf(0L, 700, 2_100, 4_900, 9_900, 14_900, 19_900), starts)
    }

    @Test
    fun `after twenty seconds of failed tries it gives up, and says how hard it tried and what it can be`() = runTest {
        val radio = FakeRadio { FakeSocket("read failed, socket might closed or timeout, read ret: -1") }
        val message = failureOf(connector(radio), car)

        // A try can start until 20 s are up.
        assertEquals(7, radio.sockets.size)
        assertTrue(radio.fakeSockets.all { it.closes.get() == 1 })
        assertEquals(
            "No Bluetooth serial link to BMW 12345 came up (7 tries over 20 s). The agent may not be running on the " +
                "head unit, the car may be out of range, or a wireless Android Auto session may be holding the car's " +
                "one serial link to this phone. Last failure: read failed, socket might closed or timeout, read ret: -1",
            message,
        )
    }

    @Test
    fun `where the tries tell why none came up, the text says that in place of what it can be`() = runTest {
        val refused = FakeRadio { FakeSocket("read failed, socket might closed or timeout, read ret: -1") }
        refused.facts = { _, _ -> TryFacts(channel = 5, carConnected = true, bonded = true) }
        val message = failureOf(connector(refused), car, dials = true)
        assertEquals(
            "No Bluetooth serial link to BMW 12345 came up (11 tries over 40 s). " + LinkCause.REFUSED.text(dials = true) +
                " Last failure: read failed, socket might closed or timeout, read ret: -1",
            message,
        )

        val noService = FakeRadio { FakeSocket("read failed") }
        noService.facts = { _, _ -> TryFacts(channel = -1, carConnected = true) }
        assertTrue(failureOf(connector(noService), car).contains(LinkCause.NO_SERVICE.text(dials = false)))

        val notConnected = FakeRadio { FakeSocket("read failed") }
        notConnected.facts = { _, _ -> TryFacts(channel = -1, carConnected = false) }
        val away = failureOf(connector(notConnected), car, dials = true)
        assertTrue(away, away.contains(LinkCause.NOT_CONNECTED.text(dials = true)))
        assertFalse(away, away.contains("agent"))
    }

    @Test
    fun `no channel where the phone cannot tell whether the car is connected gives the text of what it can be`() = runTest {
        val radio = FakeRadio { FakeSocket("read failed") }
        radio.facts = { _, _ -> TryFacts(channel = -1) }

        assertEquals(
            "No Bluetooth serial link to BMW 12345 came up (11 tries over 40 s). The agent may not be running on the head " +
                "unit, or the car may be out of range. Last failure: read failed",
            failureOf(connector(radio), car, dials = true),
        )
    }

    @Test
    fun `each try of the phone's is kept for the diagnostics, with what the phone tells of it`() = runTest {
        val radio = FakeRadio { attempt -> FakeSocket(if (attempt < 2) "refused" else null) }
        radio.facts = { _, failure -> TryFacts(errorCode = if (failure != null) 18 else null, channel = 3) }
        val diagnostics = SerialDiagnostics()
        BluetoothSerialLinkConnector(radio, now = { currentTime }, io = StandardTestDispatcher(testScheduler), diagnostics = diagnostics, listener = SerialListener(radio, Dispatchers.IO))
            .connect(car)

        val dials = diagnostics.tries().filter { it.path == LinkTry.Path.DIAL }
        assertEquals(listOf(LinkTry.Outcome.FAILED, LinkTry.Outcome.UP), dials.map { it.outcome })
        assertEquals("IOException", dials[0].error)
        assertEquals("refused", dials[0].message)
        assertEquals(TryFacts(errorCode = 18, channel = 3), dials[0].facts)
        assertEquals(TryFacts(channel = 3), dials[1].facts)
    }

    @Test
    fun `a head unit whose agent dials is tried for 40 seconds, one not known to for 20`() = runTest {
        val notKnown = FakeRadio { FakeSocket("refused") }
        assertTrue(failureOf(connector(notKnown), car, dials = false).contains("(7 tries over 20 s)"))
        assertEquals(19_900, currentTime)

        val began = currentTime
        val dials = FakeRadio { FakeSocket("refused") }
        assertTrue(failureOf(connector(dials), car, dials = true).contains("(11 tries over 40 s)"))
        assertEquals(39_900, currentTime - began)
    }

    @Test
    fun `a try that hears nothing is given 15 seconds, and its socket is closed`() = runTest {
        // The car never answers: each connect blocks until its socket is closed, on a thread of its own.
        val radio = FakeRadio { FakeSocket(blocks = true) }
        radio.listenFailure = "Bluetooth is off"
        val message = failureOf(connector(radio, io = Dispatchers.IO), car)

        // Tries at 0 and 15.7 s, each given up after 15 s: no third could start inside the 20 s.
        assertEquals(2, radio.sockets.size)
        assertTrue(radio.fakeSockets.all { it.closes.get() >= 1 })
        assertEquals(30_700, currentTime)
        assertTrue(message, message.contains("(2 tries over 31 s)"))
        assertTrue(message, message.endsWith("Last failure: The car didn't answer within 15 s"))
        assertTrue(message, message.contains(LinkCause.NO_ANSWER.text(dials = false)))
    }

    @Test
    fun `for an agent that dials, a give-up that cannot tell why does not put it down to another app holding the link`() = runTest {
        val radio = FakeRadio { FakeSocket("read failed") }
        val message = failureOf(connector(radio), car, dials = true)

        assertEquals(
            "No Bluetooth serial link to BMW 12345 came up (11 tries over 40 s). The agent may not be running on the head " +
                "unit, or the car may be out of range. Last failure: read failed",
            message,
        )
    }

    @Test
    fun `a failure that gives no words of its own leaves none in the text`() = runTest {
        val radio = object : SerialRadio {
            override fun unavailable(address: String): String? = null
            override fun socket(address: String, uuid: UUID): SerialSocket = throw IOException()
        }
        val message = failureOf(connector(radio, times = SerialConnectTimes(windowMillis = 500)), car)

        assertTrue(message, message.startsWith("No Bluetooth serial link to BMW 12345 came up (1 try). "))
        assertTrue(message, message.endsWith("serial link to this phone."))
    }

    @Test
    fun `a caller cancelled just as the link comes up does not leave the socket open`() = runTest {
        // The connect goes through, and the cancel is there before the caller is handed the link.
        lateinit var caller: Job
        val socket = object : SerialSocket {
            var closes = 0
            override val input = ByteArrayInputStream(ByteArray(0))
            override val output = ByteArrayOutputStream()
            override fun connect() = caller.cancel()
            override fun close() {
                closes++
            }
        }
        val radio = object : SerialRadio {
            override fun unavailable(address: String): String? = null
            override fun socket(address: String, uuid: UUID): SerialSocket = socket
        }
        val connector = connector(radio)
        caller = launch { connector.connect(car) }
        caller.join()

        assertTrue(caller.isCancelled)
        assertTrue("the socket was left open", socket.closes >= 1)
    }

    @Test
    fun `a link whose socket fails to close is closed all the same`() = runTest {
        val radio = FakeRadio()
        val link = connector(radio).connect(car)
        radio.fakeSockets.single().closeFailure = "already gone"
        link.close()
        assertEquals(1, radio.fakeSockets.single().closes.get())
    }

    @Test
    fun `listening that cannot start leaves the phone's dial to go on alone`() = runTest {
        val radio = FakeRadio { attempt -> FakeSocket(if (attempt < 2) "refused" else null) }
        radio.listenFailure = "The phone refused to listen"
        // The listener on the test's clock too, as it blocks on nothing here: it has failed before the second try.
        val link = connector(radio, listenIo = StandardTestDispatcher(testScheduler)).connect(car, dials = true)

        assertSame(radio.sockets.last().output, link.output)
        assertEquals(1, radio.listens.size)
    }

    // ---- a connect that only listens ------------------------------------------------------------------------------

    @Test
    fun `a connect that only listens and has no link from the car fails after 40 seconds, with no dial made`() = runTest {
        val radio = FakeRadio()
        val diagnostics = SerialDiagnostics()
        val listener = SerialListener(radio, Dispatchers.IO)
        val connector = BluetoothSerialLinkConnector(radio, now = { currentTime }, io = StandardTestDispatcher(testScheduler), diagnostics = diagnostics, listener = listener)

        val failure = try {
            connector.connect(car, dials = true, listenOnly = true)
            fail("the connect went through")
            null
        } catch (e: NoCarLinkException) {
            e
        }

        assertEquals(40_000, currentTime)
        assertTrue(radio.sockets.isEmpty())
        val text = failure?.message.orEmpty()
        assertTrue(text, text.startsWith("The car didn't connect to this app within 40 s."))
        assertTrue(text, text.contains("The app waits for the car here") && text.contains("every 10 to 30 seconds"))
        val waited = diagnostics.tries().single { it.path == LinkTry.Path.LISTEN_ONLY }
        assertEquals(LinkTry.Outcome.NO_LINK, waited.outcome)
        assertEquals(40_000L, waited.millis)
        // Nothing is left listening.
        val began = System.nanoTime()
        while (!listener.idle && System.nanoTime() - began < 10_000_000_000) Thread.sleep(5)
        assertTrue(listener.idle)
    }

    @Test
    fun `a connect that only listens and cannot listen dials the car, as without`() = runTest {
        val radio = FakeRadio()
        radio.listenFailure = "The phone refused to listen"
        val diagnostics = SerialDiagnostics()
        val link = BluetoothSerialLinkConnector(
            radio, now = { currentTime }, io = StandardTestDispatcher(testScheduler), diagnostics = diagnostics,
            listener = SerialListener(radio, StandardTestDispatcher(testScheduler)),
        ).connect(car, dials = true, listenOnly = true)

        assertSame(radio.sockets.single().output, link.output)
        assertFalse(link.openedByCar)
        val waited = diagnostics.tries().first { it.path == LinkTry.Path.LISTEN_ONLY }
        assertEquals(LinkTry.Outcome.FAILED, waited.outcome)
        assertEquals("The phone refused to listen", waited.message)
    }

    @Test
    fun `only listening is for an agent that dials, and to one that is not known to the phone dials`() = runTest {
        val radio = FakeRadio()
        val link = connector(radio).connect(car, dials = false, listenOnly = true)
        assertSame(radio.sockets.single().output, link.output)
    }

    @Test
    fun `a connect that only listens takes the car's link, and makes no dial of its own`() = live {
        val radio = FakeRadio()
        val diagnostics = SerialDiagnostics()
        val listener = SerialListener(radio, diagnostics = diagnostics)
        val connector = BluetoothSerialLinkConnector(radio, diagnostics = diagnostics, listener = listener)
        val connecting = async { connector.connect(car, dials = true, listenOnly = true) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        val link = connecting.await()

        assertSame(incoming.output, link.output)
        assertTrue(link.magicSent && link.openedByCar)
        assertArrayEquals(FrameCodec.MAGIC, incoming.written)
        assertTrue("the phone dialled", radio.sockets.isEmpty())
        assertEquals(LinkTry.Outcome.TAKEN, diagnostics.tries().single { it.path == LinkTry.Path.LISTEN_ONLY }.outcome)
        until("the listener closed") { listener.idle }
    }

    @Test
    fun `a connect that only listens and is cancelled leaves nothing listening`() = live {
        val radio = FakeRadio()
        val live = Live(radio)
        val connecting = async { live.connector.connect(car, dials = true, listenOnly = true) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())

        connecting.cancel()
        connecting.join()

        until("the listener closed") { live.listener.idle }
        assertTrue(server.closes.get() >= 1)
        assertTrue(radio.sockets.isEmpty())
    }

    // ---- the phone's dial and the car's link, on real threads ----------------------------------------------------

    /** A connector with every time as in the app, on real threads, and its listener, to see that it is left closed. */
    private class Live(radio: SerialRadio) {
        val listener = SerialListener(radio)
        val connector = BluetoothSerialLinkConnector(radio, listener = listener)
    }

    @Test
    fun `a link the car opens is taken while the phone's dial hangs, and the dial is given up`() = live {
        val radio = FakeRadio { FakeSocket(blocks = true) }
        val live = Live(radio)
        val connecting = async { live.connector.connect(car, dials = true) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())
        // The dial runs beside the listener, and may not have made its socket yet.
        until("the phone's dial made its socket") { radio.sockets.isNotEmpty() }
        assertTrue(radio.fakeSockets.single().connecting.await(10, TimeUnit.SECONDS))

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        val link = connecting.await()

        assertSame(incoming.output, link.output)
        assertTrue(link.magicSent && link.openedByCar)
        assertArrayEquals(FrameCodec.MAGIC, incoming.written)
        assertEquals(0, incoming.closes.get())
        // Nothing else is left open: the phone's dial, and the listener.
        assertEquals(1, radio.sockets.size)
        assertTrue(radio.fakeSockets.single().closes.get() >= 1)
        until("the listener closed") { live.listener.idle }
        assertTrue(server.closes.get() >= 1)
    }

    @Test
    fun `the phone's dial wins where it comes up first, and the listener is closed`() = live {
        val radio = FakeRadio()
        val live = Live(radio)
        val link = live.connector.connect(car)

        assertSame(radio.sockets.single().output, link.output)
        assertFalse(link.openedByCar)
        until("the listener closed") { live.listener.idle }
        assertTrue(radio.servers.all { it.closes.get() >= 1 })
    }

    @Test
    fun `a link the car opens just after the phone's came up is closed, with nothing written after the magic bytes`() = live {
        val dialGate = CountDownLatch(1)
        val dialled = GatedSocket(connectGate = dialGate)
        val radio = FakeRadio { dialled }
        val live = Live(radio)
        val connecting = async { live.connector.connect(car) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())

        // The car's link is up and its magic bytes are on their way; then the phone's dial comes up and is handed over.
        val returned = CountDownLatch(1)
        val incoming = GatedSocket(writeGate = returned)
        server.dialIn(incoming, car.address)
        assertTrue(incoming.writing.await(10, TimeUnit.SECONDS))
        dialGate.countDown()
        val link = connecting.await()
        returned.countDown()

        assertSame(dialled.output, link.output)
        until("the car's link closed") { incoming.closes.get() == 1 }
        assertArrayEquals(FrameCodec.MAGIC, incoming.output.toByteArray())
        assertEquals(0, dialled.closes.get())
        until("the listener closed") { live.listener.idle }
    }

    @Test
    fun `a connect that is cancelled leaves neither the dial nor the listener open`() = live {
        val radio = FakeRadio { FakeSocket(blocks = true) }
        val live = Live(radio)
        val connecting = async { live.connector.connect(car, dials = true) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())
        // The dial runs beside the listener, and may not have made its socket yet.
        until("the phone's dial made its socket") { radio.sockets.isNotEmpty() }
        assertTrue(radio.fakeSockets.single().connecting.await(10, TimeUnit.SECONDS))

        connecting.cancel()
        connecting.join()

        assertTrue(radio.fakeSockets.single().closes.get() >= 1)
        until("the listener closed") { live.listener.idle }
        assertTrue(server.closes.get() >= 1)
        assertEquals("no second try after the cancel", 1, radio.sockets.size)
    }

    @Test
    fun `a link from another device is turned away, and the connect goes on listening for the car`() = live {
        val radio = FakeRadio { FakeSocket(blocks = true) }
        val live = Live(radio)
        val connecting = async { live.connector.connect(car, dials = true) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())

        val other = FakeSocket()
        server.dialIn(other, stranger)
        assertTrue(server.awaitAccepting())
        assertEquals(1, other.closes.get())
        assertEquals(0, other.written.size)
        assertFalse(connecting.isCompleted)

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address.lowercase())
        assertSame(incoming.output, connecting.await().output)
    }

    @Test
    fun `a dial that cannot even start leaves listening to go on alone`() = live {
        val radio = FakeRadio()
        radio.socketFailure = "The Bluetooth address saved for this head unit isn't one"
        val live = Live(radio)
        val connecting = async { live.connector.connect(car, dials = true) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        assertSame(incoming.output, connecting.await().output)
        until("the listener closed") { live.listener.idle }
    }

    // ---- the knock -----------------------------------------------------------------------------------------------

    /** A socket that keeps, on [clock], when its connect began and when it was first closed. */
    private class TimedSocket(private val clock: () -> Long, failure: String? = null) : SerialSocket {
        private val inner = FakeSocket(failure)
        val began = AtomicLong(-1)
        val closedAt = AtomicLong(-1)
        val closes get() = inner.closes.get()
        val written: ByteArray get() = inner.written
        override val input get() = inner.input
        override val output get() = inner.output

        override fun connect() {
            began.set(clock())
            inner.connect()
        }

        override fun close() {
            closedAt.compareAndSet(-1, clock())
            inner.close()
        }
    }

    private suspend fun knockFailure(
        connector: LinkConnector,
        bursts: Int? = null,
        fallback: KnockFallback = KnockFallback.ONCE_PER_KNOCK,
    ): IOException = try {
        connector.knock(car, bursts, fallback)
        throw AssertionError("the knock had a link")
    } catch (e: IOException) {
        e
    }

    private fun TestScope.knocking(radio: SerialRadio, diagnostics: SerialDiagnostics = SerialDiagnostics(), times: SerialConnectTimes = SerialConnectTimes()) =
        BluetoothSerialLinkConnector(
            radio, times, now = { currentTime }, io = StandardTestDispatcher(testScheduler), diagnostics = diagnostics,
            listener = SerialListener(radio, Dispatchers.IO),
        )

    @Test
    fun `a knock's connect that comes up is sent the knock's four bytes and nothing else, is read from never, and is closed after its hold`() = runTest {
        val sockets = CopyOnWriteArrayList<TimedSocket>()
        val radio = FakeRadio { TimedSocket({ currentTime }).also { sockets += it } }

        val failure = knockFailure(knocking(radio), bursts = 1)

        assertTrue("$failure", failure is KnockUnansweredException)
        assertEquals(5, sockets.size)
        for (socket in sockets) {
            assertArrayEquals(byteArrayOf(0x4E, 0x42, 0x54, 0x4B), socket.written)
            assertEquals(3, socket.input.available())
            assertEquals(1, socket.closes)
            // Held for the car's link for the hold's time, then closed.
            assertEquals(1_000, socket.closedAt.get() - socket.began.get())
        }
        // Each connect follows the close of the one before by the gap; the burst then waits for the car's link.
        assertEquals(listOf(0L, 1_250, 2_500, 3_750, 5_000), sockets.map { it.began.get() })
        assertEquals(7_500, currentTime)
    }

    @Test
    fun `refused knock connects are made again at the gap, five to a burst, three bursts with their pause, and all of it is kept`() = runTest {
        val starts = CopyOnWriteArrayList<Long>()
        val radio = FakeRadio {
            starts += currentTime
            FakeSocket("read failed, socket might closed or timeout, read ret: -1")
        }
        // The socket found the agent's channel on the car, which turned the requests down.
        radio.facts = { socket, _ -> TryFacts(channel = if (socket != null) 5 else null, mediaAudio = false) }
        val diagnostics = SerialDiagnostics()

        val failure = knockFailure(knocking(radio, diagnostics))

        assertTrue("$failure", failure is KnockUnansweredException)
        assertEquals(BluetoothSerialLinkConnector.KNOCK_UNANSWERED, failure.message)
        assertEquals(
            listOf<Long>(0, 250, 500, 750, 1_000, 4_500, 4_750, 5_000, 5_250, 5_500, 9_000, 9_250, 9_500, 9_750, 10_000),
            starts,
        )
        assertEquals(11_500, currentTime)
        assertTrue(radio.fakeSockets.all { it.closes.get() == 1 })
        assertTrue("nothing was written on a refused connect", radio.fakeSockets.all { it.written.isEmpty() })
        // One listener for the whole of it.
        assertEquals(1, radio.listens.size)

        val tries = diagnostics.tries()
        val knocks = tries.filter { it.path == LinkTry.Path.KNOCK }
        assertEquals(15, knocks.size)
        assertTrue(knocks.all { it.outcome == LinkTry.Outcome.REFUSED })
        assertEquals("1.1", knocks.first().detail)
        assertEquals("3.5", knocks.last().detail)
        assertEquals(listOf("1 of 3", "2 of 3", "3 of 3"), tries.filter { it.path == LinkTry.Path.KNOCK_BURST }.map { it.detail })
        assertTrue(tries.filter { it.path == LinkTry.Path.KNOCK_BURST }.all { it.outcome == LinkTry.Outcome.NO_LINK })
        val end = tries.single { it.path == LinkTry.Path.KNOCK_CONNECT }
        assertEquals(LinkTry.Outcome.NO_LINK, end.outcome)
        assertEquals(11_500L, end.millis)
        assertEquals(
            "knock 1.1 refused after 0 ms: IOException: read failed, socket might closed or timeout, read ret: -1 (channel 5, no media audio)",
            knocks.first().describe(),
        )
        assertEquals("knocking connect of 3 bursts had no link from the car after 11500 ms (no media audio)", end.describe())
    }

    @Test
    fun `a knock at a car the phone has no connection to stops after the first connect that fails, and says the car was not reached`() = runTest {
        val radio = FakeRadio { FakeSocket("read failed, socket might closed or timeout, read ret: -1") }
        // As the phone has it of a car that is off or out of range: no channel found, and no connection to the car.
        radio.facts = { _, _ -> TryFacts(channel = -1, carConnected = false, mediaAudio = false) }
        val diagnostics = SerialDiagnostics()

        val failure = knockFailure(knocking(radio, diagnostics))

        assertTrue("$failure", failure is KnockUnansweredException && failure.carAway)
        assertEquals(1, radio.sockets.size)
        // At once on the test's clock, where the knock went on for its three bursts before: 11.5 s for connects that
        // fail at once, and about 86 s for ones that each run out of their time.
        assertEquals(0, currentTime)
        assertTrue(radio.fakeSockets.all { it.closes.get() == 1 })
        val tries = diagnostics.tries()
        assertEquals(LinkTry.Outcome.FAILED, tries.single { it.path == LinkTry.Path.KNOCK }.outcome)
        assertTrue(tries.none { it.path == LinkTry.Path.KNOCK_BURST })
        assertEquals(
            "knocking connect in burst 1 of 3, after connect 1, which did not reach the car, had no link from the car after 0 ms " +
                "(no channel, car not connected, no media audio)",
            tries.single { it.path == LinkTry.Path.KNOCK_CONNECT }.describe(),
        )
    }

    @Test
    fun `a knock goes on where its connects reach the car, or the phone cannot tell, whatever the phone says of its connection`() = runTest {
        // Refused by the car: the car is there, though the phone has no hands-free or audio connection to it.
        val refused = FakeRadio { FakeSocket("refused") }
        refused.facts = { socket, _ -> TryFacts(channel = if (socket != null) 5 else null, carConnected = false) }
        val failure = knockFailure(knocking(refused), bursts = 1)
        assertTrue("$failure", failure is KnockUnansweredException && !failure.carAway)
        assertEquals(5, refused.sockets.size)

        // Failed on the phone while it has a connection to the car, or where it cannot tell.
        for (connected in listOf(true, null)) {
            val failing = FakeRadio { FakeSocket("Connection refused") }
            failing.facts = { _, _ -> TryFacts(carConnected = connected) }
            val failed = knockFailure(knocking(failing), bursts = 1)
            assertTrue("$connected: $failed", failed is KnockUnansweredException && !failed.carAway)
            assertEquals("$connected", 5, failing.sockets.size)
        }

        // Came up: the knock was sent, whatever the phone said of its connection before.
        val up = FakeRadio()
        up.facts = { _, _ -> TryFacts(carConnected = false) }
        val unanswered = knockFailure(knocking(up), bursts = 1)
        assertTrue("$unanswered", unanswered is KnockUnansweredException && !unanswered.carAway)
        assertEquals(5, up.sockets.size)
    }

    @Test
    fun `a car that the first connect reaches gets its knock through, though the phone had no connection to it before`() = live {
        val radio = FakeRadio()
        radio.facts = { _, _ -> TryFacts(carConnected = false) }
        val listener = SerialListener(radio)
        val connector = BluetoothSerialLinkConnector(radio, SerialConnectTimes(knockHoldMillis = 600_000), listener = listener)
        val knocking = async { connector.knock(car) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())
        until("the knock was sent") { radio.fakeSockets.firstOrNull()?.written?.size == 4 }

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        val link = knocking.await()

        assertSame(incoming.output, link.output)
        assertEquals(1, radio.sockets.size)
        until("the listener closed") { listener.idle }
    }

    @Test
    fun `a knock's connect that fails on the phone is kept as such and the burst goes on, and one that hears nothing ends the knock`() = live {
        // On real threads: the connect that hears nothing blocks one until its time is up.
        val radio = FakeRadio { attempt -> if (attempt == 3) FakeSocket(blocks = true) else FakeSocket("Connection refused") }
        val diagnostics = SerialDiagnostics()
        val connector = BluetoothSerialLinkConnector(
            radio, SerialConnectTimes(knockTryMillis = 300, knockSettleMillis = 50), diagnostics = diagnostics, listener = SerialListener(radio),
        )

        val failure = knockFailure(connector)

        // No answer in its time: the car was not reached, and no connect follows, of this burst or another.
        assertTrue("$failure", failure is KnockUnansweredException && failure.carAway)
        val knocks = diagnostics.tries().filter { it.path == LinkTry.Path.KNOCK }
        assertEquals(listOf(LinkTry.Outcome.FAILED, LinkTry.Outcome.FAILED, LinkTry.Outcome.NO_ANSWER), knocks.map { it.outcome })
        assertTrue(radio.fakeSockets.all { it.closes.get() >= 1 })
        assertEquals(3, radio.sockets.size)

        // A socket the phone cannot even make is a connect that failed.
        val none = FakeRadio()
        none.socketFailure = "The Bluetooth address saved for this head unit isn't one"
        val kept = SerialDiagnostics()
        val quick = BluetoothSerialLinkConnector(none, SerialConnectTimes(knockSettleMillis = 50), diagnostics = kept, listener = SerialListener(none))
        assertTrue(knockFailure(quick, bursts = 1) is KnockUnansweredException)
        assertEquals(5, kept.tries().count { it.path == LinkTry.Path.KNOCK && it.outcome == LinkTry.Outcome.FAILED })
    }

    @Test
    fun `a knock's connect that hears nothing while the phone is connected to the car does not end the knock`() = live {
        // On real threads: the connect that hears nothing blocks one until its time is up.
        val radio = FakeRadio { attempt -> if (attempt == 1) FakeSocket(blocks = true) else FakeSocket("Connection refused") }
        // The car is there: the phone holds a connection to it, and the car left the first request without an answer.
        radio.facts = { _, _ -> TryFacts(carConnected = true) }
        val diagnostics = SerialDiagnostics()
        val connector = BluetoothSerialLinkConnector(
            radio, SerialConnectTimes(knockTryMillis = 300, knockSettleMillis = 50), diagnostics = diagnostics, listener = SerialListener(radio),
        )

        val failure = knockFailure(connector, bursts = 1)

        // The burst went on to its last connect, and the knock failed as one the car did not answer, not as a car away.
        assertTrue("$failure", failure is KnockUnansweredException && !failure.carAway)
        val knocks = diagnostics.tries().filter { it.path == LinkTry.Path.KNOCK }
        assertEquals(LinkTry.Outcome.NO_ANSWER, knocks.first().outcome)
        assertEquals(5, knocks.size)
        assertEquals(5, radio.sockets.size)
        assertTrue(radio.fakeSockets.all { it.closes.get() >= 1 })
    }

    @Test
    fun `a knock where the phone cannot listen fails as a link that cannot come, and is no unanswered knock`() = runTest {
        val radio = FakeRadio { FakeSocket("refused") }
        radio.listenFailure = "The phone refused to listen"
        val connector = BluetoothSerialLinkConnector(
            radio, now = { currentTime }, io = StandardTestDispatcher(testScheduler), listener = SerialListener(radio, StandardTestDispatcher(testScheduler)),
        )

        val failure = knockFailure(connector)

        assertTrue("$failure", failure is NoCarLinkException && failure !is KnockUnansweredException)
        assertEquals("This phone can't listen for the car's link: The phone refused to listen", failure.message)
        assertTrue(radio.fakeSockets.all { it.closes.get() >= 1 })
    }

    @Test
    fun `a knock to a car Bluetooth cannot reach fails at once, with nothing dialled or listened on`() = runTest {
        val radio = FakeRadio()
        radio.unavailable = "Bluetooth is off"
        assertEquals("Bluetooth is off", knockFailure(knocking(radio)).message)
        assertTrue(radio.sockets.isEmpty() && radio.listens.isEmpty())
    }

    @Test
    fun `the car's link ends the burst the moment it is handed over, and is the link`() = live {
        val radio = FakeRadio { FakeSocket("refused") }
        val diagnostics = SerialDiagnostics()
        val listener = SerialListener(radio, diagnostics = diagnostics)
        // A gap no test sits out: the burst ends within it, or the test runs out of time.
        val connector = BluetoothSerialLinkConnector(radio, SerialConnectTimes(knockGapMillis = 600_000), diagnostics = diagnostics, listener = listener)
        val knocking = async { connector.knock(car) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())
        until("the first knock") { radio.fakeSockets.firstOrNull()?.closes?.get() == 1 }

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        val link = knocking.await()

        assertSame(incoming.output, link.output)
        assertTrue(link.openedByCar && link.magicSent)
        assertArrayEquals(FrameCodec.MAGIC, incoming.written)
        assertEquals("no knock after the car's link", 1, radio.sockets.size)
        val end = diagnostics.tries().single { it.path == LinkTry.Path.KNOCK_CONNECT }
        assertEquals(LinkTry.Outcome.TAKEN, end.outcome)
        assertEquals("in burst 1 of 3, after connect 1", end.detail)
        until("the listener closed") { listener.idle }
        assertEquals(1, radio.listens.size)
    }

    @Test
    fun `a knock's connect that is up is closed when the car's link comes, not at the end of its hold, and has one line`() = live {
        val radio = FakeRadio()
        val diagnostics = SerialDiagnostics()
        val listener = SerialListener(radio, diagnostics = diagnostics)
        val connector = BluetoothSerialLinkConnector(radio, SerialConnectTimes(knockHoldMillis = 600_000), diagnostics = diagnostics, listener = listener)
        val knocking = async { connector.knock(car) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())
        until("the knock was sent") { radio.fakeSockets.firstOrNull()?.written?.size == 4 }
        assertEquals(0, radio.fakeSockets.single().closes.get())

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        val link = knocking.await()

        assertSame(incoming.output, link.output)
        val knock = radio.fakeSockets.single()
        assertEquals(1, knock.closes.get())
        assertArrayEquals(FrameCodec.KNOCK_MAGIC, knock.written)
        assertEquals(3, knock.input.available())
        until("the listener closed") { listener.idle }
        // Its hold was cancelled by the car's link: the line it had stays the only one, not followed by a cancel.
        val tries = diagnostics.tries()
        assertEquals(listOf(LinkTry.Outcome.KNOCKED), tries.filter { it.path == LinkTry.Path.KNOCK }.map { it.outcome })
        assertEquals(listOf(LinkTry.Outcome.TAKEN), tries.filter { it.path == LinkTry.Path.KNOCK_CONNECT }.map { it.outcome })
    }

    @Test
    fun `a knock that is cancelled leaves no connect of its own, and nothing listening, wherever it is`() = live {
        // In a connect that hangs.
        val hanging = FakeRadio { FakeSocket(blocks = true) }
        val first = Live(hanging)
        val knocking = async { first.connector.knock(car) }
        until("the knock's connect") { hanging.fakeSockets.firstOrNull()?.connecting?.count == 0L }
        knocking.cancel()
        knocking.join()
        assertEquals(1, hanging.sockets.size)
        assertTrue(hanging.fakeSockets.single().closes.get() >= 1)
        until("the listener closed") { first.listener.idle }
        assertTrue(hanging.servers.all { it.closes.get() >= 1 })

        // In the hold of one that is up.
        val up = FakeRadio()
        val listener = SerialListener(up)
        val connector = BluetoothSerialLinkConnector(up, SerialConnectTimes(knockHoldMillis = 600_000), listener = listener)
        val held = async { connector.knock(car) }
        until("the knock was sent") { up.fakeSockets.firstOrNull()?.written?.size == 4 }
        held.cancel()
        held.join()
        assertEquals(1, up.sockets.size)
        assertTrue(up.fakeSockets.single().closes.get() >= 1)
        until("the listener closed") { listener.idle }

        // In the gap between two.
        val refused = FakeRadio { FakeSocket("refused") }
        val gapListener = SerialListener(refused)
        val gapped = BluetoothSerialLinkConnector(refused, SerialConnectTimes(knockGapMillis = 600_000), listener = gapListener)
        val waiting = async { gapped.knock(car) }
        until("the first knock") { refused.fakeSockets.firstOrNull()?.closes?.get() == 1 }
        waiting.cancel()
        waiting.join()
        assertEquals(1, refused.sockets.size)
        until("the listener closed") { gapListener.idle }
    }

    /** A socket whose connect blocks until [gate] opens, and that a close does not end: one stuck in the phone's stack. */
    private class StuckSocket(private val gate: CountDownLatch) : SerialSocket {
        val connecting = CountDownLatch(1)
        val closes = AtomicInteger()
        override val input = ByteArrayInputStream(ByteArray(0))
        override val output = ByteArrayOutputStream()

        override fun connect() {
            connecting.countDown()
            gate.await(10, TimeUnit.SECONDS)
        }

        override fun close() {
            closes.incrementAndGet()
        }
    }

    @Test
    fun `a car's link handed to a knock whose caller is cancelled while its own connect is being ended is closed`() = live {
        val gate = CountDownLatch(1)
        val stuck = StuckSocket(gate)
        val radio = FakeRadio { stuck }
        val diagnostics = SerialDiagnostics()
        val listener = SerialListener(radio, diagnostics = diagnostics)
        val connector = BluetoothSerialLinkConnector(radio, SerialConnectTimes(knockTryMillis = 600_000), diagnostics = diagnostics, listener = listener)
        try {
            val knocking = async { connector.knock(car) }
            val server = radio.nextServer()
            assertTrue(server.awaitAccepting())
            assertTrue("the knock's connect never began", stuck.connecting.await(10, TimeUnit.SECONDS))

            // The car's link is handed over, and the knock's connect is given up: the knock waits for it to end.
            val incoming = FakeSocket()
            server.dialIn(incoming, car.address)
            until("the knock's connect was given up") { stuck.closes.get() >= 1 }
            knocking.cancel()
            gate.countDown()
            knocking.join()

            assertTrue(knocking.isCancelled)
            assertTrue("the car's link was left open", incoming.closes.get() >= 1)
            until("the listener closed") { listener.idle }
            assertEquals(1, radio.sockets.size)
            // One line for the connect, and one for the knock: both given up.
            val tries = diagnostics.tries()
            assertEquals(listOf(LinkTry.Outcome.CANCELLED), tries.filter { it.path == LinkTry.Path.KNOCK }.map { it.outcome })
            assertEquals(listOf(LinkTry.Outcome.CANCELLED), tries.filter { it.path == LinkTry.Path.KNOCK_CONNECT }.map { it.outcome })
        } finally {
            gate.countDown()
        }
    }

    @Test
    fun `a car's link that won a connect whose caller is cancelled while the phone's dial is being ended is closed`() = live {
        val gate = CountDownLatch(1)
        val stuck = StuckSocket(gate)
        val radio = FakeRadio { stuck }
        val live = Live(radio)
        try {
            val connecting = async { live.connector.connect(car, dials = true) }
            val server = radio.nextServer()
            assertTrue(server.awaitAccepting())
            assertTrue("the phone's dial never began", stuck.connecting.await(10, TimeUnit.SECONDS))

            val incoming = FakeSocket()
            server.dialIn(incoming, car.address)
            until("the phone's dial was given up") { stuck.closes.get() >= 1 }
            connecting.cancel()
            gate.countDown()
            connecting.join()

            assertTrue(connecting.isCancelled)
            assertTrue("the car's link was left open", incoming.closes.get() >= 1)
            until("the listener closed") { live.listener.idle }
            assertEquals("no second try after the cancel", 1, radio.sockets.size)
        } finally {
            gate.countDown()
        }
    }

    @Test
    fun `a knock shares the listener a wait at rest has open, and its car's link goes to the knock`() = live {
        val radio = FakeRadio { FakeSocket("refused") }
        val listener = SerialListener(radio)
        val connector = BluetoothSerialLinkConnector(radio, SerialConnectTimes(knockGapMillis = 600_000), listener = listener)
        val atRest = async { listener.accept(listOf(car.address)) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())

        val knocking = async { connector.knock(car) }
        until("the first knock") { radio.fakeSockets.firstOrNull()?.closes?.get() == 1 }
        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)

        assertSame(incoming.output, knocking.await().output)
        assertFalse(atRest.isCompleted)
        assertEquals("a second listener was opened", 1, radio.listens.size)
        atRest.cancel()
        atRest.join()
        until("the listener closed") { listener.idle }
    }

    // ---- the knock through the car's Serial Port service ----------------------------------------------------------

    /**
     * A stand-in for the phone's Bluetooth that tells a knock's connects apart by the service they are to: [own] makes
     * the sockets to the agent's service and [standard] those to the car's Serial Port service, each by its count
     * there, and what the phone tells of each is the test's ([ownFacts], [standardFacts]). Listening is [base]'s.
     */
    private class KnockRadio(
        val base: FakeRadio = FakeRadio(),
        private val own: (attempt: Int) -> SerialSocket = { FakeSocket() },
        private val standard: (attempt: Int) -> SerialSocket = { FakeSocket() },
    ) : SerialRadio by base {
        /** The service of each socket asked for, in order. */
        val asked = CopyOnWriteArrayList<UUID>()
        val ownSockets = CopyOnWriteArrayList<SerialSocket>()
        val standardSockets = CopyOnWriteArrayList<SerialSocket>()

        @Volatile
        var ownFacts: (failure: Throwable?) -> TryFacts = { TryFacts() }

        @Volatile
        var standardFacts: (failure: Throwable?) -> TryFacts = { TryFacts() }

        override fun socket(address: String, uuid: UUID): SerialSocket {
            asked += uuid
            return when (uuid) {
                OWN -> own(ownSockets.size + 1).also { ownSockets += it }
                STANDARD -> standard(standardSockets.size + 1).also { standardSockets += it }
                else -> throw AssertionError("a connect to $uuid")
            }
        }

        override fun inspect(address: String, socket: SerialSocket?, failure: Throwable?): TryFacts = when {
            // The knock's own last line, which is about no socket.
            socket == null -> TryFacts()
            socket in standardSockets -> standardFacts(failure)
            else -> ownFacts(failure)
        }
    }

    /** A log that opens [latch] at the first line that starts with [start]: for a test to wait until a knock's connect has its line. */
    private fun opening(latch: CountDownLatch, start: String) = LinkLog { if (it.startsWith(start)) latch.countDown() }

    private fun knockLines(diagnostics: SerialDiagnostics) = diagnostics.tries().filter { it.path == LinkTry.Path.KNOCK }

    private fun knockEnd(diagnostics: SerialDiagnostics) = diagnostics.tries().single { it.path == LinkTry.Path.KNOCK_CONNECT }

    @Test
    fun `a knock whose connects to the agent's service come up makes none to the Serial Port service`() = runTest {
        val radio = KnockRadio()

        val failure = knockFailure(knocking(radio))

        assertTrue("$failure", failure is KnockUnansweredException && !failure.carAway)
        assertEquals(List(15) { OWN }, radio.asked)
    }

    @Test
    fun `a connect to the agent's service that fails turns the next to the Serial Port service, whether the phone found its channel, found none, or cannot tell`() = runTest {
        for ((channel, outcome) in listOf(-1 to LinkTry.Outcome.FAILED, 5 to LinkTry.Outcome.REFUSED, null to LinkTry.Outcome.FAILED)) {
            val radio = KnockRadio(own = { FakeSocket("read failed") }, standard = { FakeSocket("read failed") })
            radio.ownFacts = { TryFacts(channel = channel, carConnected = true) }
            radio.standardFacts = { TryFacts(channel = 1, carConnected = true) }
            val diagnostics = SerialDiagnostics()

            val failure = knockFailure(knocking(radio, diagnostics), bursts = 1)

            assertTrue("$channel: $failure", failure is KnockUnansweredException && !failure.carAway)
            // The two there in the places of the next two, and the rest at the agent's service again.
            assertEquals("$channel", listOf(OWN, STANDARD, STANDARD, OWN, OWN), radio.asked)
            val knocks = knockLines(diagnostics)
            assertEquals("$channel", outcome, knocks[0].outcome)
            assertEquals(
                listOf("1.1", "1.2 to the Serial Port service", "1.3 to the Serial Port service", "1.4", "1.5"),
                knocks.map { it.detail },
            )
        }
    }

    @Test
    fun `where both connects to the Serial Port service fail, the knock goes on at the agent's service to the end of its bursts, as it does today`() = runTest {
        val starts = CopyOnWriteArrayList<Long>()
        val radio = KnockRadio(
            own = {
                starts += currentTime
                FakeSocket("read failed, socket might closed or timeout, read ret: -1")
            },
            standard = {
                starts += currentTime
                FakeSocket("read failed, socket might closed or timeout, read ret: -1")
            },
        )
        // The car refuses them all: its one link for phone apps is held, say.
        radio.ownFacts = { TryFacts(channel = 5, carConnected = true) }
        radio.standardFacts = { TryFacts(channel = 1, carConnected = true) }
        val diagnostics = SerialDiagnostics()

        val failure = knockFailure(knocking(radio, diagnostics))

        assertTrue("$failure", failure is KnockUnansweredException && !failure.carAway)
        // As many connects, at the same times, as a knock that makes none there.
        assertEquals(listOf(OWN, STANDARD, STANDARD) + List(12) { OWN }, radio.asked)
        assertEquals(
            listOf<Long>(0, 250, 500, 750, 1_000, 4_500, 4_750, 5_000, 5_250, 5_500, 9_000, 9_250, 9_500, 9_750, 10_000),
            starts,
        )
        assertEquals(11_500, currentTime)
        assertTrue(radio.ownSockets.plus(radio.standardSockets).all { (it as FakeSocket).closes.get() == 1 })
        val knocks = knockLines(diagnostics)
        assertEquals(15, knocks.size)
        assertTrue(knocks.all { it.outcome == LinkTry.Outcome.REFUSED })
        assertEquals(
            "knock 1.2 to the Serial Port service refused after 0 ms: IOException: read failed, socket might closed or timeout, " +
                "read ret: -1 (channel 1, car connected)",
            knocks[1].describe(),
        )
        assertEquals("3.5", knocks.last().detail)
        assertEquals(3, diagnostics.tries().count { it.path == LinkTry.Path.KNOCK_BURST })
        assertEquals("knocking connect of 3 bursts had no link from the car after 11500 ms", knockEnd(diagnostics).describe())
    }

    @Test
    fun `a connect to the Serial Port service that comes up and brings no link is the only one there, and the knock goes on at the agent's service`() = runTest {
        // As at a car where no agent runs: another program there may take the link to its Serial Port service.
        val sockets = CopyOnWriteArrayList<TimedSocket>()
        val radio = KnockRadio(
            own = { FakeSocket("read failed, socket might closed or timeout, read ret: -1") },
            standard = { TimedSocket({ currentTime }).also { sockets += it } },
        )
        radio.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        val diagnostics = SerialDiagnostics()

        val failure = knockFailure(knocking(radio, diagnostics))

        assertTrue("$failure", failure is KnockUnansweredException && !failure.carAway)
        // Though every connect to the agent's service fails, none goes there again.
        assertEquals(listOf(OWN, STANDARD) + List(13) { OWN }, radio.asked)
        val through = sockets.single()
        assertArrayEquals(FrameCodec.KNOCK_MAGIC, through.written)
        assertEquals(1, through.closes)
        // Held for the hold's time; the bursts then go on as they would have.
        assertEquals(250, through.began.get())
        assertEquals(1_250, through.closedAt.get())
        assertEquals(12_500, currentTime)
        assertEquals(LinkTry.Outcome.KNOCKED, knockLines(diagnostics)[1].outcome)
        assertEquals("of 3 bursts", knockEnd(diagnostics).detail)
    }

    @Test
    fun `a connect to the Serial Port service brings the car's link, which is taken`() = live {
        val failed = FakeSocket("read failed, socket might closed or timeout, read ret: -1")
        val through = FakeSocket()
        val radio = KnockRadio(own = { failed }, standard = { through })
        // The phone cannot tell the channel: the turn to the Serial Port service does not ask.
        radio.ownFacts = { TryFacts(carConnected = true) }
        radio.standardFacts = { TryFacts(channel = 1, carConnected = true) }
        val diagnostics = SerialDiagnostics()
        val held = CountDownLatch(1)
        val connector = BluetoothSerialLinkConnector(
            radio, SerialConnectTimes(knockHoldMillis = 600_000), diagnostics = diagnostics,
            log = opening(held, "knock 1.2 to the Serial Port service came up"), listener = SerialListener(radio),
        )
        val knocking = async { connector.knock(car) }
        val server = radio.base.nextServer()
        assertTrue(server.awaitAccepting())
        // The connect to the Serial Port service is up, with the knock sent, and holds for the car's link.
        assertTrue("the knock through the Serial Port service was never sent", held.await(10, TimeUnit.SECONDS))

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        val link = knocking.await()

        assertSame(incoming.output, link.output)
        assertTrue(link.openedByCar)
        assertEquals(listOf(OWN, STANDARD), radio.asked)
        assertEquals(1, failed.closes.get())
        assertTrue("nothing was written on the connect that failed", failed.written.isEmpty())
        assertArrayEquals(FrameCodec.KNOCK_MAGIC, through.written)
        assertEquals(3, through.input.available())
        assertTrue("the knock's connect was left open", through.closes.get() >= 1)
        assertTrue("the listener was left open", server.closes.get() >= 1)
        val knocks = knockLines(diagnostics)
        assertEquals(listOf("1.1", "1.2 to the Serial Port service"), knocks.map { it.detail })
        assertEquals(listOf(LinkTry.Outcome.FAILED, LinkTry.Outcome.KNOCKED), knocks.map { it.outcome })
        assertTrue(knocks[1].describe(), knocks[1].describe().endsWith("(channel 1, car connected)"))
        val end = knockEnd(diagnostics)
        assertEquals(LinkTry.Outcome.TAKEN, end.outcome)
        assertEquals("in burst 1 of 3, after connect 2 to the Serial Port service", end.detail)
    }

    @Test
    fun `a connect to the agent's service after those to the Serial Port service brings the car's link, which is taken`() = live {
        // The first to the agent's service fails, as one does while another app's link to the car is up; the next one
        // there comes up, and is held until the car's link comes.
        val up = FakeSocket()
        val radio = KnockRadio(own = { attempt -> if (attempt == 1) FakeSocket("read failed") else up }, standard = { FakeSocket("read failed") })
        radio.ownFacts = { failure -> TryFacts(channel = if (failure != null) 5 else null, carConnected = true) }
        radio.standardFacts = { TryFacts(channel = 1, carConnected = true) }
        val diagnostics = SerialDiagnostics()
        val held = CountDownLatch(1)
        val connector = BluetoothSerialLinkConnector(
            radio, SerialConnectTimes(knockHoldMillis = 600_000), diagnostics = diagnostics,
            log = opening(held, "knock 1.4 came up"), listener = SerialListener(radio),
        )
        val knocking = async { connector.knock(car) }
        val server = radio.base.nextServer()
        assertTrue(server.awaitAccepting())
        assertTrue("the knock at the agent's service was never sent", held.await(10, TimeUnit.SECONDS))

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        val link = knocking.await()

        assertSame(incoming.output, link.output)
        assertEquals(listOf(OWN, STANDARD, STANDARD, OWN), radio.asked)
        assertArrayEquals(FrameCodec.KNOCK_MAGIC, up.written)
        assertTrue("the knock's connect was left open", up.closes.get() >= 1)
        assertEquals("in burst 1 of 3, after connect 4", knockEnd(diagnostics).detail)
    }

    @Test
    fun `a knock at a car that its first connect does not reach makes no connect to the Serial Port service, and one there that does not reach it ends the knock`() = runTest {
        // No channel found and no connection to the car: the car is away.
        val away = KnockRadio(own = { FakeSocket("read failed") })
        away.ownFacts = { TryFacts(channel = -1, carConnected = false) }
        val first = knockFailure(knocking(away))
        assertTrue("$first", first is KnockUnansweredException && first.carAway)
        assertEquals(listOf(OWN), away.asked)
        assertEquals(0, currentTime)

        // The car went out of reach between the connect to the agent's service and the one to the Serial Port service.
        val gone = KnockRadio(own = { FakeSocket("read failed") }, standard = { FakeSocket("read failed") })
        gone.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        gone.standardFacts = { TryFacts(channel = -1, carConnected = false) }
        val diagnostics = SerialDiagnostics()
        val second = knockFailure(knocking(gone, diagnostics))
        assertTrue("$second", second is KnockUnansweredException && second.carAway)
        assertEquals(listOf(OWN, STANDARD), gone.asked)
        assertEquals(
            "in burst 1 of 3, after connect 2 to the Serial Port service, which did not reach the car,",
            knockEnd(diagnostics).detail,
        )
    }

    @Test
    fun `a knock told to make none there, or allowed none, makes no connect to the Serial Port service`() = runTest {
        // Told so, as the wait after an agent's update is.
        val told = KnockRadio(own = { FakeSocket("read failed") })
        told.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        val failure = knockFailure(knocking(told), fallback = KnockFallback.NONE)
        assertTrue("$failure", failure is KnockUnansweredException && !failure.carAway)
        assertEquals(List(15) { OWN }, told.asked)

        // Allowed none.
        val none = KnockRadio(own = { FakeSocket("read failed") })
        none.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        knockFailure(knocking(none, times = SerialConnectTimes(knockStandardAttempts = 0)))
        assertEquals(List(15) { OWN }, none.asked)
    }

    // ---- once in each burst, as for a connect the user starts -----------------------------------------------------

    @Test
    fun `a knock that may go there once in each burst, where every connect is refused, begins each later burst there and ends as today`() = runTest {
        val starts = CopyOnWriteArrayList<Long>()
        val radio = KnockRadio(
            own = {
                starts += currentTime
                FakeSocket("refused")
            },
            standard = {
                starts += currentTime
                FakeSocket("refused")
            },
        )
        radio.ownFacts = { TryFacts(channel = 5, carConnected = true) }
        radio.standardFacts = { TryFacts(channel = 1, carConnected = true) }
        val diagnostics = SerialDiagnostics()

        val failure = knockFailure(knocking(radio, diagnostics), fallback = KnockFallback.ONCE_PER_BURST)

        assertTrue("$failure", failure is KnockUnansweredException && !failure.carAway)
        // The first burst looks at the agent's service first; the later ones begin at the Serial Port service at once.
        assertEquals(
            listOf(OWN, STANDARD, STANDARD, OWN, OWN) + listOf(STANDARD, STANDARD, OWN, OWN, OWN) + listOf(STANDARD, STANDARD, OWN, OWN, OWN),
            radio.asked,
        )
        assertEquals(9, radio.ownSockets.size)
        assertEquals(6, radio.standardSockets.size)
        // As many connects, at the same times, as a knock that makes none there, and the same end.
        assertEquals(
            listOf<Long>(0, 250, 500, 750, 1_000, 4_500, 4_750, 5_000, 5_250, 5_500, 9_000, 9_250, 9_500, 9_750, 10_000),
            starts,
        )
        assertEquals(11_500, currentTime)
        assertEquals("2.1 to the Serial Port service", knockLines(diagnostics)[5].detail)
        assertEquals("knocking connect of 3 bursts had no link from the car after 11500 ms", knockEnd(diagnostics).describe())
    }

    @Test
    fun `a knock that may go there once in each burst makes one connect there that comes up in each burst, at most`() = runTest {
        // As at a car where no agent runs: each connect to the Serial Port service comes up, and nothing answers.
        val sockets = CopyOnWriteArrayList<TimedSocket>()
        val radio = KnockRadio(
            own = { FakeSocket("read failed") },
            standard = { TimedSocket({ currentTime }).also { sockets += it } },
        )
        radio.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        val diagnostics = SerialDiagnostics()

        val failure = knockFailure(knocking(radio, diagnostics), fallback = KnockFallback.ONCE_PER_BURST)

        assertTrue("$failure", failure is KnockUnansweredException && !failure.carAway)
        assertEquals(
            listOf(OWN, STANDARD, OWN, OWN, OWN) + listOf(STANDARD, OWN, OWN, OWN, OWN) + listOf(STANDARD, OWN, OWN, OWN, OWN),
            radio.asked,
        )
        assertEquals(3, sockets.size)
        assertTrue(sockets.all { it.closes == 1 && it.written.contentEquals(FrameCodec.KNOCK_MAGIC) })
        // Each held for the hold's time; the later bursts' at their start.
        assertEquals(listOf<Long>(250, 5_500, 11_000), sockets.map { it.began.get() })
        assertEquals(14_500, currentTime)
    }

    @Test
    fun `a knock that may go there once in each burst takes the car's link that its connect there in the second burst brings`() = live {
        // The two there in the first burst are refused; the one that begins the second comes up, and holds for the link.
        val up = FakeSocket()
        val radio = KnockRadio(own = { FakeSocket("read failed") }, standard = { attempt -> if (attempt <= 2) FakeSocket("refused") else up })
        radio.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        radio.standardFacts = { TryFacts(channel = 1, carConnected = true) }
        val diagnostics = SerialDiagnostics()
        val held = CountDownLatch(1)
        // No wait after a burst and between two that a test sits out, and a hold that only the car's link ends.
        val connector = BluetoothSerialLinkConnector(
            radio, SerialConnectTimes(knockHoldMillis = 600_000, knockSettleMillis = 0, knockPauseMillis = 0), diagnostics = diagnostics,
            log = opening(held, "knock 2.1 to the Serial Port service came up"), listener = SerialListener(radio),
        )
        val knocking = async { connector.knock(car, fallback = KnockFallback.ONCE_PER_BURST) }
        val server = radio.base.nextServer()
        assertTrue(server.awaitAccepting())
        assertTrue("the second burst's knock through the Serial Port service was never sent", held.await(10, TimeUnit.SECONDS))

        val incoming = FakeSocket()
        server.dialIn(incoming, car.address)
        val link = knocking.await()

        assertSame(incoming.output, link.output)
        assertEquals(listOf(OWN, STANDARD, STANDARD, OWN, OWN, STANDARD), radio.asked)
        assertArrayEquals(FrameCodec.KNOCK_MAGIC, up.written)
        assertTrue("the knock's connect was left open", up.closes.get() >= 1)
        assertEquals("in burst 2 of 3, after connect 1 to the Serial Port service", knockEnd(diagnostics).detail)
    }

    @Test
    fun `a knock that may go there once in each burst makes none there where the agent's service answers, or where none is allowed`() = runTest {
        val works = KnockRadio()
        knockFailure(knocking(works), fallback = KnockFallback.ONCE_PER_BURST)
        assertEquals(List(15) { OWN }, works.asked)

        val none = KnockRadio(own = { FakeSocket("read failed") })
        none.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        knockFailure(knocking(none, times = SerialConnectTimes(knockStandardAttempts = 0)), fallback = KnockFallback.ONCE_PER_BURST)
        assertEquals(List(15) { OWN }, none.asked)
    }

    @Test
    fun `a knock that may go there once in each burst ends where a connect of either service does not reach the car`() = runTest {
        // The first, to the agent's service.
        val away = KnockRadio(own = { FakeSocket("read failed") })
        away.ownFacts = { TryFacts(channel = -1, carConnected = false) }
        val first = knockFailure(knocking(away), fallback = KnockFallback.ONCE_PER_BURST)
        assertTrue("$first", first is KnockUnansweredException && first.carAway)
        assertEquals(listOf(OWN), away.asked)

        // The one that begins the second burst, to the Serial Port service.
        var standards = 0
        val gone = KnockRadio(own = { FakeSocket("read failed") }, standard = { FakeSocket("read failed") })
        gone.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        gone.standardFacts = { TryFacts(channel = -1, carConnected = ++standards <= 2) }
        val diagnostics = SerialDiagnostics()
        val second = knockFailure(knocking(gone, diagnostics), fallback = KnockFallback.ONCE_PER_BURST)
        assertTrue("$second", second is KnockUnansweredException && second.carAway)
        assertEquals(listOf(OWN, STANDARD, STANDARD, OWN, OWN, STANDARD), gone.asked)
        assertEquals(
            "in burst 2 of 3, after connect 1 to the Serial Port service, which did not reach the car,",
            knockEnd(diagnostics).detail,
        )
    }

    @Test
    fun `a knock that is cancelled in its connect to the Serial Port service leaves it closed, and nothing listening`() = live {
        val hanging = FakeSocket(blocks = true)
        val radio = KnockRadio(own = { FakeSocket("read failed") }, standard = { hanging })
        radio.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        val diagnostics = SerialDiagnostics()
        val connector = BluetoothSerialLinkConnector(radio, diagnostics = diagnostics, listener = SerialListener(radio))
        val knocking = async { connector.knock(car) }
        val server = radio.base.nextServer()
        assertTrue(server.awaitAccepting())
        assertTrue("the connect to the Serial Port service never began", hanging.connecting.await(10, TimeUnit.SECONDS))

        knocking.cancel()
        knocking.join()

        assertTrue(knocking.isCancelled)
        assertTrue("the connect was left open", hanging.closes.get() >= 1)
        assertEquals(listOf(OWN, STANDARD), radio.asked)
        assertTrue("the listener was left open", server.closes.get() >= 1)
        val knocks = knockLines(diagnostics)
        assertEquals(listOf(LinkTry.Outcome.FAILED, LinkTry.Outcome.CANCELLED), knocks.map { it.outcome })
        assertEquals("1.2 to the Serial Port service", knocks[1].detail)
        assertEquals(LinkTry.Outcome.CANCELLED, knockEnd(diagnostics).outcome)
    }

    @Test
    fun `a knock that is cancelled in the hold of its connect to the Serial Port service leaves it closed, with the one line it had`() = live {
        val through = FakeSocket()
        val radio = KnockRadio(own = { FakeSocket("read failed") }, standard = { through })
        radio.ownFacts = { TryFacts(channel = -1, carConnected = true) }
        val diagnostics = SerialDiagnostics()
        val held = CountDownLatch(1)
        val connector = BluetoothSerialLinkConnector(
            radio, SerialConnectTimes(knockHoldMillis = 600_000), diagnostics = diagnostics,
            log = opening(held, "knock 1.2 to the Serial Port service came up"), listener = SerialListener(radio),
        )
        val knocking = async { connector.knock(car) }
        val server = radio.base.nextServer()
        assertTrue(server.awaitAccepting())
        assertTrue("the knock through the Serial Port service was never sent", held.await(10, TimeUnit.SECONDS))

        knocking.cancel()
        knocking.join()

        assertTrue(knocking.isCancelled)
        assertTrue("the connect was left open", through.closes.get() >= 1)
        assertArrayEquals(FrameCodec.KNOCK_MAGIC, through.written)
        assertEquals(listOf(OWN, STANDARD), radio.asked)
        assertTrue("the listener was left open", server.closes.get() >= 1)
        assertEquals(listOf(LinkTry.Outcome.FAILED, LinkTry.Outcome.KNOCKED), knockLines(diagnostics).map { it.outcome })
        assertEquals(LinkTry.Outcome.CANCELLED, knockEnd(diagnostics).outcome)
    }

    @Test
    fun `a caller that is cancelled while a connect blocks gets out of it, and the socket is closed`() = runBlocking {
        // On real threads: the connect sits in a blocking call that only the socket's close ends.
        val socket = FakeSocket(blocks = true)
        val radio = FakeRadio { socket }
        val callers = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val connecting = callers.async { BluetoothSerialLinkConnector(radio).connect(car) }
            assertTrue("the connect never started", socket.connecting.await(10, TimeUnit.SECONDS))
            connecting.cancel()

            assertNotNull("the cancelled connect never came back", withTimeoutOrNull(10_000) { connecting.join() })
            assertTrue(connecting.isCancelled)
            assertTrue(socket.closes.get() >= 1)
            assertEquals("no second try after the cancel", 1, radio.sockets.size)
        } finally {
            // Lets go of a connect that is still blocked, so that a failure here cannot hold the test run up.
            socket.close()
            callers.cancel()
        }
    }
}

class LinkConnectorsTest {

    private class Named(val name: String, val reaches: (Endpoint) -> Boolean) : LinkConnector {
        val dials = CopyOnWriteArrayList<Boolean>()
        val listenOnly = CopyOnWriteArrayList<Boolean>()
        override fun supports(endpoint: Endpoint) = reaches(endpoint)
        override suspend fun connect(endpoint: Endpoint): Link = MemoryLink.pair(kind = name).first
        override suspend fun connect(endpoint: Endpoint, dials: Boolean): Link {
            this.dials += dials
            return connect(endpoint)
        }

        override suspend fun connect(endpoint: Endpoint, dials: Boolean, listenOnly: Boolean): Link {
            this.listenOnly += listenOnly
            return connect(endpoint, dials)
        }
    }

    private val serial = SerialEndpoint("00:11:22:33:44:55", "BMW 12345")
    private val tcp = TcpEndpoint("192.168.0.1", 7144)

    @Test
    fun `an endpoint goes to the first connector that reaches its kind`() = runBlocking {
        val both = LinkConnectors(listOf(Named("serial") { it is SerialEndpoint }, Named("tcp") { it is TcpEndpoint }, Named("late") { true }))
        assertTrue(both.supports(serial) && both.supports(tcp))
        assertEquals("serial", both.connect(serial).kind)
        assertEquals("tcp", both.connect(tcp).kind)
    }

    @Test
    fun `whether the agent dials is handed on`() = runBlocking {
        val serialOnly = Named("serial") { it is SerialEndpoint }
        LinkConnectors(listOf(serialOnly)).connect(serial, dials = true)
        assertEquals(listOf(true), serialOnly.dials)
        LinkConnectors(listOf(serialOnly)).connect(serial, dials = true, listenOnly = true)
        assertEquals(listOf(true, true), serialOnly.dials)
        assertEquals(listOf(true), serialOnly.listenOnly)
    }

    @Test
    fun `a knock is handed on, and a connector that cannot knock connects as to an agent that dials`() = runBlocking {
        val knocks = CopyOnWriteArrayList<Int?>()
        val fallbacks = CopyOnWriteArrayList<KnockFallback>()
        val knocking = object : LinkConnector by Named("serial", { it is SerialEndpoint }) {
            override suspend fun knock(endpoint: Endpoint, bursts: Int?, fallback: KnockFallback): Link {
                knocks += bursts
                fallbacks += fallback
                return MemoryLink.pair(kind = "knocked").first
            }
        }
        val tcpOnly = Named("tcp") { it is TcpEndpoint }
        val both = LinkConnectors(listOf(knocking, tcpOnly))

        assertEquals("knocked", both.knock(serial, 1).kind)
        assertEquals("knocked", both.knock(serial).kind)
        assertEquals("knocked", both.knock(serial, 1, KnockFallback.NONE).kind)
        assertEquals("knocked", both.knock(serial, fallback = KnockFallback.ONCE_PER_BURST).kind)
        assertEquals(listOf<Int?>(1, null, 1, null), knocks)
        // A caller that does not say has the cautious kind.
        assertEquals(listOf(KnockFallback.ONCE_PER_KNOCK, KnockFallback.ONCE_PER_KNOCK, KnockFallback.NONE, KnockFallback.ONCE_PER_BURST), fallbacks)
        assertEquals("tcp", both.knock(tcp).kind)
        assertEquals(listOf(true), tcpOnly.dials)
    }

    @Test
    fun `an endpoint none of them reaches is not supported, and connecting to it fails`() = runBlocking {
        val serialOnly = LinkConnectors(listOf(Named("serial") { it is SerialEndpoint }))
        assertFalse(serialOnly.supports(tcp))
        try {
            serialOnly.connect(tcp)
            fail("connected")
        } catch (e: IOException) {
            assertTrue(e.message.orEmpty().contains("no way"))
        }
    }
}

/** The agent's own serial service, and the car's standard Serial Port service, as a knock's connects go to them. */
private val OWN: UUID = BluetoothSerialLinkConnector.SERVICE_UUID
private val STANDARD: UUID = UUID.fromString("00001101-0000-1000-8000-00805f9b34fb")

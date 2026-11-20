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

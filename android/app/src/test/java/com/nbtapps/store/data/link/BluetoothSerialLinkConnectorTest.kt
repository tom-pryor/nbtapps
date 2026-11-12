// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
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

/** The serial connector against a stand-in for the phone's Bluetooth. */
@OptIn(ExperimentalCoroutinesApi::class)
class BluetoothSerialLinkConnectorTest {

    private val car = SerialEndpoint("00:11:22:33:44:55", "BMW 12345")

    private class FakeSocket(
        private val failure: String?,
        private val blocks: Boolean = false,
        /** When set, the link comes up and then takes no bytes. */
        private val writeFailure: String? = null,
    ) : SerialSocket {
        val closes = AtomicInteger()
        val connecting = CountDownLatch(1)
        private val closed = CountDownLatch(1)

        @Volatile
        var closeFailure: String? = null

        override val input = ByteArrayInputStream(byteArrayOf(1, 2, 3))
        override val output = object : ByteArrayOutputStream() {
            override fun write(bytes: ByteArray, offset: Int, length: Int) {
                writeFailure?.let { throw IOException(it) }
                super.write(bytes, offset, length)
            }
        }

        override fun connect() {
            connecting.countDown()
            // Like the real one, a connect under way ends only when the socket is closed.
            if (blocks) {
                closed.await()
                throw IOException("socket closed")
            }
            failure?.let { throw IOException(it) }
        }

        override fun close() {
            closes.incrementAndGet()
            closed.countDown()
            closeFailure?.let { throw IOException(it) }
        }
    }

    private class FakeRadio(private val socketFor: (attempt: Int) -> FakeSocket) : SerialRadio {
        @Volatile
        var unavailable: String? = null
        val asked = CopyOnWriteArrayList<Pair<String, UUID>>()
        val sockets = CopyOnWriteArrayList<FakeSocket>()

        override fun unavailable(address: String): String? = unavailable

        override fun socket(address: String, uuid: UUID): SerialSocket {
            asked += address to uuid
            return socketFor(sockets.size + 1).also { sockets += it }
        }
    }

    /** A connector on the test's own clock: a pause between tries takes no real time. */
    private fun TestScope.connector(radio: FakeRadio) =
        BluetoothSerialLinkConnector(radio, now = { currentTime }, io = StandardTestDispatcher(testScheduler))

    private suspend fun failureOf(connector: LinkConnector, endpoint: Endpoint): String = try {
        connector.connect(endpoint)
        fail("the connect went through")
        ""
    } catch (e: IOException) {
        e.message.orEmpty()
    }

    @Test
    fun `reaches Bluetooth serial endpoints only`() = runTest {
        val connector = connector(FakeRadio { FakeSocket(null) })
        assertTrue(connector.supports(car))
        assertFalse(connector.supports(TcpEndpoint("192.168.0.1", 7144)))
        assertTrue(failureOf(connector, TcpEndpoint("192.168.0.1", 7144)).isNotEmpty())
    }

    @Test
    fun `dials the agent's service on the endpoint's device, and the link is a serial one over that socket`() = runTest {
        val radio = FakeRadio { FakeSocket(null) }
        val link = connector(radio).connect(car)

        assertEquals(listOf("00:11:22:33:44:55" to UUID.fromString("58876f35-7f3f-4592-b65d-9912589a4f51")), radio.asked)
        val socket = radio.sockets.single()
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
        val radio = FakeRadio { FakeSocket(null) }
        val link = connector(radio).connect(car)

        // Nothing has been asked of the link yet: the agent sees how it opens without waiting for a first frame.
        assertArrayEquals(FrameCodec.MAGIC, radio.sockets.single().output.toByteArray())
        assertTrue(link.magicSent)
        assertFalse(MemoryLink.pair(kind = Link.SERIAL).first.magicSent)
    }

    @Test
    fun `a link that comes up and cannot take the magic bytes is a try that failed`() = runTest {
        val radio = FakeRadio { attempt -> FakeSocket(null, writeFailure = if (attempt < 3) "Broken pipe" else null) }
        val link = connector(radio).connect(car)

        assertEquals(3, radio.sockets.size)
        assertEquals(listOf(1, 1, 0), radio.sockets.map { it.closes.get() })
        assertSame(radio.sockets.last().output, link.output)
        assertArrayEquals(FrameCodec.MAGIC, radio.sockets.last().output.toByteArray())
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
    fun `Bluetooth that cannot be used fails at once with the reason, and nothing is dialled`() = runTest {
        val radio = FakeRadio { FakeSocket(null) }
        radio.unavailable = "Bluetooth is off"
        assertEquals("Bluetooth is off", failureOf(connector(radio), car))
        assertTrue(radio.sockets.isEmpty())
        assertEquals(0, currentTime)
    }

    @Test
    fun `a connect that fails is tried again on a new socket, and the failed ones are closed`() = runTest {
        // Another app holds the car's one serial link for the first three tries.
        val radio = FakeRadio { attempt -> FakeSocket(if (attempt < 4) "read failed, socket might closed or timeout" else null) }
        val link = connector(radio).connect(car)

        assertEquals(4, radio.sockets.size)
        assertEquals(listOf(1, 1, 1, 0), radio.sockets.map { it.closes.get() })
        assertEquals(3 * BluetoothSerialLinkConnector.CONNECT_PAUSE_MILLIS, currentTime)
        assertSame(radio.sockets.last().input, link.input)
    }

    @Test
    fun `after twenty seconds of failed tries it gives up, and says how hard it tried and what it can be`() = runTest {
        val radio = FakeRadio { FakeSocket("read failed, socket might closed or timeout, read ret: -1") }
        val message = failureOf(connector(radio), car)

        // Tries every 700 ms while one can still start inside the 20 s.
        assertEquals(29, radio.sockets.size)
        assertTrue(radio.sockets.all { it.closes.get() == 1 })
        assertEquals(
            "No Bluetooth serial link to BMW 12345 came up (29 tries over 20 s). The agent may not be running on the " +
                "head unit, the car may be out of range, or a wireless Android Auto session may be holding the car's " +
                "one serial link to this phone. Last failure: read failed, socket might closed or timeout, read ret: -1",
            message,
        )
    }

    @Test
    fun `a failure that gives no words of its own leaves none in the text`() = runTest {
        val radio = object : SerialRadio {
            override fun unavailable(address: String): String? = null
            override fun socket(address: String, uuid: UUID): SerialSocket = throw IOException()
        }
        val message = failureOf(BluetoothSerialLinkConnector(radio, windowMillis = 500, now = { currentTime }, io = StandardTestDispatcher(testScheduler)), car)

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
        val connector = BluetoothSerialLinkConnector(radio, now = { currentTime }, io = StandardTestDispatcher(testScheduler))
        caller = launch { connector.connect(car) }
        caller.join()

        assertTrue(caller.isCancelled)
        assertTrue("the socket was left open", socket.closes >= 1)
    }

    @Test
    fun `a link whose socket fails to close is closed all the same`() = runTest {
        val radio = FakeRadio { FakeSocket(null) }
        val link = connector(radio).connect(car)
        radio.sockets.single().closeFailure = "already gone"
        link.close()
        assertEquals(1, radio.sockets.single().closes.get())
    }

    @Test
    fun `a caller that is cancelled while a connect blocks gets out of it, and the socket is closed`() = runBlocking {
        // On real threads: the connect sits in a blocking call that only the socket's close ends.
        val socket = FakeSocket(null, blocks = true)
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
        override fun supports(endpoint: Endpoint) = reaches(endpoint)
        override suspend fun connect(endpoint: Endpoint): Link = MemoryLink.pair(kind = name).first
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

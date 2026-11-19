// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.withTestTimeout
import java.io.IOException
import java.util.UUID
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** The phone's one listener for the links a car opens to it, against a stand-in for the phone's Bluetooth, on real threads. */
class SerialListenerTest {

    private val car = "00:11:22:33:44:55"
    private val other = "66:77:88:99:AA:BB"
    private val radio = FakeRadio()
    private val listener = SerialListener(radio)

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTestTimeout(60_000, block) }

    private suspend fun until(what: String, condition: () -> Boolean) {
        withTimeoutOrNull(10_000) { while (!condition()) delay(5) } ?: throw AssertionError("never came: $what")
    }

    @Test
    fun `a link from the car waited for is handed over, as a serial link with the magic bytes written`() = test {
        val waiting = async { listener.accept(listOf(car)) }
        val server = radio.nextServer()
        assertEquals(listOf(ProtocolConstants.SERIAL_SERVICE_NAME to UUID.fromString(ProtocolConstants.SERIAL_SERVICE_UUID)), radio.listens)

        val socket = FakeSocket()
        server.dialIn(socket, car)
        val accepted = waiting.await()

        assertEquals(car, accepted.address)
        assertEquals(Link.SERIAL, accepted.link.kind)
        assertTrue(accepted.link.magicSent)
        assertSame(socket.output, accepted.link.output)
        assertArrayEquals(FrameCodec.MAGIC, socket.written)
        assertEquals(0, socket.closes.get())
        // Nobody waits any more, so nothing listens.
        until("the listener closed") { server.closes.get() >= 1 }
        assertFalse(listener.listening)

        accepted.link.close()
        assertEquals(1, socket.closes.get())
    }

    @Test
    fun `an address is the same one in either case`() = test {
        val waiting = async { listener.accept(listOf(car.lowercase())) }
        radio.nextServer().dialIn(FakeSocket(), car)
        assertEquals(car, waiting.await().address)
    }

    @Test
    fun `a link from another device is closed at once, with nothing written, and listening goes on`() = test {
        val waiting = async { listener.accept(listOf(car)) }
        val server = radio.nextServer()
        val stranger = FakeSocket()
        server.dialIn(stranger, other)
        assertTrue(server.awaitAccepting())
        assertTrue(server.awaitAccepting())
        assertEquals(1, stranger.closes.get())
        assertEquals(0, stranger.written.size)
        assertFalse(waiting.isCompleted)

        val socket = FakeSocket()
        server.dialIn(socket, car)
        assertEquals(car, waiting.await().address)
        assertEquals(1, radio.servers.size)
    }

    @Test
    fun `a link that cannot take the magic bytes is closed, and listening goes on`() = test {
        val waiting = async { listener.accept(listOf(car)) }
        val server = radio.nextServer()
        val broken = FakeSocket(writeFailure = "Broken pipe")
        server.dialIn(broken, car)
        assertTrue(server.awaitAccepting())
        assertTrue(server.awaitAccepting())
        assertEquals(1, broken.closes.get())

        server.dialIn(FakeSocket(), car)
        assertEquals(car, waiting.await().address)
    }

    @Test
    fun `a caller that gives up closes the listener, which ends the accept under way, and nothing stays open`() = test {
        val waiting = async { listener.accept(listOf(car)) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())

        waiting.cancel()
        waiting.join()

        until("the listener closed") { server.closes.get() >= 1 }
        assertFalse(listener.listening)
        // Safe to close again.
        server.close()

        // The next caller has a listener of its own.
        val again = async { listener.accept(listOf(car)) }
        val next = radio.nextServer()
        next.dialIn(FakeSocket(), car)
        assertEquals(car, again.await().address)
        assertEquals(2, radio.servers.size)
    }

    @Test
    fun `listening that cannot start fails the caller with the reason`() = test {
        radio.listenFailure = "Bluetooth is off"
        try {
            listener.accept(listOf(car))
            fail("accepted")
        } catch (e: IOException) {
            assertEquals("Bluetooth is off", e.message)
        }
        assertFalse(listener.listening)
    }

    @Test
    fun `listening that fails of itself fails everyone waiting, and the listener is closed`() = test {
        val waiting = async { runCatching { listener.accept(listOf(car)) } }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())

        server.fail("Bluetooth went off")

        assertEquals("Bluetooth went off", waiting.await().exceptionOrNull()?.message)
        assertEquals(1, server.closes.get())
    }

    @Test
    fun `two callers share one listener, and a link both would take goes to the one that comes first`() = test {
        val atRest = async { listener.accept(listOf(car, other)) }
        val server = radio.nextServer()
        assertTrue(server.awaitAccepting())
        // Waiting by the time it returns.
        val connecting = async(start = CoroutineStart.UNDISPATCHED) { listener.accept(listOf(car), first = true) }

        // Each link to the one that takes its address, the connect's first.
        server.dialIn(FakeSocket(), car)
        assertEquals(car, connecting.await().address)
        server.dialIn(FakeSocket(), other)
        assertEquals(other, atRest.await().address)
        assertEquals(1, radio.servers.size)
    }
}

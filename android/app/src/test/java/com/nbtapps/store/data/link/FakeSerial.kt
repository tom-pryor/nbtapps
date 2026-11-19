// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/** A stand-in for one RFCOMM socket, dialled or accepted. */
class FakeSocket(
    private val failure: String? = null,
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

    /** What was written to it, from the first byte. */
    val written: ByteArray get() = output.toByteArray()

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

/**
 * A stand-in for the phone's listener. [accept] blocks, on a real thread, until the test brings a link in ([dialIn]),
 * the listener is closed, or the test fails it ([fail]).
 */
class FakeServer : SerialServer {
    private val queue = LinkedBlockingQueue<Any>()
    val closes = AtomicInteger()

    /** One item each time [accept] is entered, for a test to wait on. */
    val accepting = LinkedBlockingQueue<Unit>()

    /** A device at [address] opens a link to the phone: [socket], up. */
    fun dialIn(socket: SerialSocket, address: String) = queue.put(AcceptedSocket(socket, address))

    /** Listening fails of itself, as it does when Bluetooth goes off. */
    fun fail(message: String) = queue.put(IOException(message))

    override fun accept(): AcceptedSocket {
        accepting.put(Unit)
        return when (val next = queue.take()) {
            is AcceptedSocket -> next
            is IOException -> throw next
            else -> {
                // Closed: so is every accept after this one.
                queue.put(next)
                throw IOException("server closed")
            }
        }
    }

    override fun close() {
        closes.incrementAndGet()
        queue.put(Unit)
    }

    /** Waits for [accept] to be entered once more; false if it was not within ten seconds. */
    fun awaitAccepting(): Boolean = accepting.poll(10, TimeUnit.SECONDS) != null
}

/** A stand-in for the phone's Bluetooth: dials hand out [FakeSocket]s, and listening hands out [FakeServer]s. */
class FakeRadio(private val socketFor: (attempt: Int) -> SerialSocket = { FakeSocket() }) : SerialRadio {
    @Volatile
    var unavailable: String? = null
    val asked = CopyOnWriteArrayList<Pair<String, UUID>>()
    val sockets = CopyOnWriteArrayList<SerialSocket>()

    /** When set, [socket] throws this. */
    @Volatile
    var socketFailure: String? = null

    /** When set, listening cannot start, and says this. */
    @Volatile
    var listenFailure: String? = null

    /** Each listen, with the name and UUID it was asked for. */
    val listens = CopyOnWriteArrayList<Pair<String, UUID>>()
    val servers = CopyOnWriteArrayList<FakeServer>()

    /** Each server as it is made, for a test to wait on. */
    val serverMade = LinkedBlockingQueue<FakeServer>()

    override fun unavailable(address: String): String? = unavailable

    override fun socket(address: String, uuid: UUID): SerialSocket {
        asked += address to uuid
        socketFailure?.let { throw IOException(it) }
        return socketFor(sockets.size + 1).also { sockets += it }
    }

    override fun listen(name: String, uuid: UUID): SerialServer {
        listens += name to uuid
        listenFailure?.let { throw IOException(it) }
        return FakeServer().also {
            servers += it
            serverMade.put(it)
        }
    }

    /** The next server made, within ten seconds. */
    fun nextServer(): FakeServer = serverMade.poll(10, TimeUnit.SECONDS) ?: throw AssertionError("nothing listened")

    /** The dial sockets as [FakeSocket]s. */
    val fakeSockets: List<FakeSocket> get() = sockets.map { it as FakeSocket }
}

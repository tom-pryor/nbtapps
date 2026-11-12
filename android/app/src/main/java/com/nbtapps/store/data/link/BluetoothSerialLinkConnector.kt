// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.ConnectFailedException
import com.nbtapps.store.data.connectWithRetry
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.SerialEndpoint
import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** What the connector needs of the phone's Bluetooth, kept apart from it so that the rest runs in a unit test. */
interface SerialRadio {
    /**
     * Why no serial link to the device at [address] can be tried at the moment, in words for the user: no
     * permission, Bluetooth off, not paired. Null if one can.
     */
    fun unavailable(address: String): String?

    /** A socket to the service [uuid] on the paired device at [address], not yet connected. */
    fun socket(address: String, uuid: UUID): SerialSocket
}

/** One RFCOMM socket. */
interface SerialSocket : Closeable {
    /**
     * Blocks until the link is up.
     *
     * @throws IOException if it does not come up, or [close] is called meanwhile.
     */
    fun connect()

    val input: InputStream
    val output: OutputStream

    /** Ends the link, or a [connect] under way. Safe to call twice, and from any thread. */
    override fun close()
}

/**
 * Bluetooth serial (RFCOMM) to the agent's own service on the car. The phone dials, so it speaks first; the
 * agent writes nothing until a valid `hello` has come, because every serial link a phone opens to the car lands
 * on the same node there, another app's included.
 *
 * What the phone says first is how the agent tells its links from those of other programs on the car: it waits
 * [ProtocolConstants.SERIAL_DECIDE_MS] from its open of a link for the magic bytes and lets the link go without
 * them. So they are written here, the moment the connect comes back, and not with the first frame, which waits
 * for whoever sends it. The link says so ([Link.magicSent]), and its [Peer] sends none.
 *
 * The car takes one serial link from a phone at a time. While another app on the phone holds it, a connect
 * fails at once, so a failed connect is tried again for [windowMillis]. During a wireless Android Auto session
 * that link is held throughout, and serial is not to be had.
 *
 * The socket is the authenticated, encrypted kind, so the link is to the car the phone is paired with, and
 * nobody listens in. That does not say who opened it: any app on a paired phone can. The agent therefore asks
 * for `auth` on this link, with a key it issued when the user typed a code from the car's screen into this app
 * (`JobRepository`, `PairingKeys`).
 */
class BluetoothSerialLinkConnector(
    private val radio: SerialRadio,
    private val windowMillis: Long = CONNECT_WINDOW_MILLIS,
    private val pauseMillis: Long = CONNECT_PAUSE_MILLIS,
    private val now: () -> Long = ::monotonicMillis,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) : LinkConnector {

    override fun supports(endpoint: Endpoint): Boolean = endpoint is SerialEndpoint

    override suspend fun connect(endpoint: Endpoint): Link {
        val serial = endpoint as? SerialEndpoint ?: throw IOException("Not a Bluetooth serial endpoint")
        radio.unavailable(serial.address)?.let { throw IOException(it) }
        try {
            return SerialLink(connectWithRetry(windowMillis, pauseMillis, now) { dial(serial.address) })
        } catch (e: ConnectFailedException) {
            throw IOException(gaveUp(serial, e), e)
        }
    }

    /**
     * What the user is told when no try came up. The phone learns only that the connect failed, not why, so the
     * text names what it can be; Android's own words for the failure come last, for whoever can read them.
     */
    private fun gaveUp(serial: SerialEndpoint, failure: ConnectFailedException): String =
        "No Bluetooth serial link to ${serial.deviceName} came up (${failure.tries}). The agent may not be running " +
            "on the head unit, the car may be out of range, or a wireless Android Auto session may be holding the " +
            "car's one serial link to this phone." + failure.cause?.message?.let { " Last failure: $it" }.orEmpty()

    /** One try. A connect blocks for seconds and hears of nothing but its socket closing, so a caller that is cancelled closes it. */
    private suspend fun dial(address: String): SerialSocket {
        var made: SerialSocket? = null
        try {
            return withContext(io) { radio.socket(address, SERVICE_UUID).also { made = it }.also { bringUp(it) } }
        } catch (e: CancellationException) {
            // Cancelled, perhaps at the very moment the link came up: nobody is handed the socket, so it must not
            // stay open and hold the car's one serial link.
            made?.closeQuietly()
            throw e
        }
    }

    private suspend fun bringUp(socket: SerialSocket) {
        // Set once the connect has come back, either way: from then on the socket is not the watcher's to close.
        val settled = AtomicBoolean(false)
        try {
            coroutineScope {
                // Started here and now, not handed to the dispatcher: a cancel that came before it had begun to
                // run would skip it, and the connect would sit out its time.
                val onCancel = launch(start = CoroutineStart.UNDISPATCHED) {
                    try {
                        awaitCancellation()
                    } finally {
                        if (!settled.get()) socket.closeQuietly()
                    }
                }
                try {
                    socket.connect()
                    // Part of bringing the link up: one that cannot take them is a try that failed.
                    socket.output.write(FrameCodec.MAGIC)
                    socket.output.flush()
                } finally {
                    settled.set(true)
                }
                onCancel.cancel()
            }
        } catch (e: IOException) {
            socket.closeQuietly()
            // A connect that failed because its caller was cancelled is a cancellation, not a try that failed.
            currentCoroutineContext().ensureActive()
            throw e
        }
    }

    private class SerialLink(private val socket: SerialSocket) : Link {
        override val kind = Link.SERIAL
        override val input: InputStream = socket.input
        override val output: OutputStream = socket.output
        override val magicSent = true

        /** Closing the socket also fails a read that is blocked on it. */
        override fun close() = socket.closeQuietly()
    }

    companion object {
        private fun SerialSocket.closeQuietly() {
            try {
                close()
            } catch (e: IOException) {
                // It is gone either way.
            }
        }

        val SERVICE_UUID: UUID = UUID.fromString(ProtocolConstants.SERIAL_SERVICE_UUID)

        /** How long a connect is tried again: longer than another app holds the car's link unanswered. */
        const val CONNECT_WINDOW_MILLIS = 20_000L
        const val CONNECT_PAUSE_MILLIS = 700L
    }
}

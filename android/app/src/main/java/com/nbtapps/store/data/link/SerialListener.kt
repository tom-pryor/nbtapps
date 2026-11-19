// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import java.io.IOException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/** A link a head unit opened to the phone: the Bluetooth [address] it came from, and the [link], whose magic bytes have gone out. */
class AcceptedLink(val address: String, val link: Link)

/**
 * The app's one listener on the agent's Bluetooth serial service, for the links the car opens to the phone: an agent
 * that lists [ProtocolConstants.CAPABILITY_SERIAL_DIAL] has the car dial a phone of its own on the agent's UUID, so
 * that another program holding the car's one link for phone-opened links does not keep the app out.
 *
 * Whoever wants such a link waits in [accept], for the addresses it takes one from: a connect for its car, the app
 * at rest for every car it has saved. However many wait, there is one listener on the phone, open while anyone waits
 * and closed when the last of them is done, and one link goes to one of them. A link from an address nobody waits
 * for is closed at once and listening goes on.
 *
 * What the phone says first is how the agent tells the app from anything else that answers on its UUID: it waits a
 * few seconds from the car's open for the magic bytes, and without them ends the link and leaves the phone alone for
 * a while. So they are written the moment a link is accepted, as they are the moment a connect comes back
 * ([BluetoothSerialLinkConnector]); a link that cannot take them is closed, and listening goes on.
 *
 * [accept] blocks on [io], and is ended by closing the listener; nothing here stays open once nobody waits.
 */
class SerialListener(
    private val radio: SerialRadio,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) {
    private class Waiter(val addresses: Set<String>) {
        val result = CompletableDeferred<AcceptedLink>()
    }

    private val lock = Any()

    /** Who waits, the one to be given a link first at the front. Guarded by [lock]. */
    private val waiters = ArrayList<Waiter>()

    /** The listener that is open. Null while none is, and once it is being closed. Guarded by [lock]. */
    private var server: SerialServer? = null

    /** Whether [serve] runs. Guarded by [lock]. */
    private var serving = false

    private val scope = CoroutineScope(SupervisorJob() + io)

    /**
     * Waits for a head unit at one of [addresses] to open a link to the phone, and returns it with its magic bytes
     * written. The addresses are compared without regard to case. With [first], this one is given a link that others
     * waiting would take too: a connect's, ahead of the app's own at rest.
     *
     * A caller that is cancelled leaves nothing open: a link handed over at that moment is closed.
     *
     * @throws IOException if the phone cannot listen, or stops being able to, as when Bluetooth is switched off.
     */
    @OptIn(ExperimentalCoroutinesApi::class)
    suspend fun accept(addresses: Collection<String>, first: Boolean = false): AcceptedLink {
        val waiter = Waiter(addresses.map { it.uppercase() }.toSet())
        synchronized(lock) {
            if (first) waiters.add(0, waiter) else waiters += waiter
            if (!serving) {
                serving = true
                scope.launch { serve() }
            }
        }
        try {
            return waiter.result.await()
        } catch (e: CancellationException) {
            // Handed over just as the caller gave up: nobody has the link, and on serial it must not stay open.
            if (!waiter.result.completeExceptionally(e) && waiter.result.getCompletionExceptionOrNull() == null) {
                waiter.result.getCompleted().link.close()
            }
            throw e
        } finally {
            leave(waiter)
        }
    }

    /** Whether anyone waits, and so whether the listener is open or about to be. */
    val listening: Boolean get() = synchronized(lock) { waiters.isNotEmpty() }

    /** Nobody waits, and the listener is closed and no longer accepted on: for a test to see that nothing is left open. */
    internal val idle: Boolean get() = synchronized(lock) { waiters.isEmpty() && !serving }

    private fun leave(waiter: Waiter) {
        val closing = synchronized(lock) {
            waiters.remove(waiter)
            if (waiters.isNotEmpty()) return
            server.also { server = null }
        }
        // Ends the accept under way: [serve] sees the listener is no longer the open one, and stops.
        closing?.closeQuietly()
    }

    /** Fails everyone waiting with [failure], since the phone cannot listen: they are no longer waiting. */
    private fun failAll(failure: IOException) {
        val failed = synchronized(lock) { waiters.toList().also { waiters.clear() } }
        failed.forEach { it.result.completeExceptionally(failure) }
    }

    /** Opens the listener and accepts on it for as long as anyone waits. One runs at a time. */
    private fun serve() {
        while (true) {
            synchronized(lock) {
                if (waiters.isEmpty()) {
                    serving = false
                    return
                }
            }
            val opened = try {
                radio.listen(ProtocolConstants.SERIAL_SERVICE_NAME, BluetoothSerialLinkConnector.SERVICE_UUID)
            } catch (e: IOException) {
                failAll(e)
                continue
            }
            val kept = synchronized(lock) {
                // Not where everyone left while it was being opened.
                waiters.isNotEmpty().also { if (it) server = opened }
            }
            if (!kept) {
                opened.closeQuietly()
                continue
            }
            acceptOn(opened)
        }
    }

    /** Accepts on [opened] until it is closed or fails. */
    private fun acceptOn(opened: SerialServer) {
        while (true) {
            val incoming = try {
                opened.accept()
            } catch (e: IOException) {
                // Closed by [leave], or failed of itself, as when Bluetooth goes off: then nobody can be given a link.
                val failed = synchronized(lock) { (server === opened).also { if (it) server = null } }
                opened.closeQuietly()
                if (failed) failAll(e)
                return
            }
            handOver(incoming)
        }
    }

    /** Gives [incoming] to the first who waits for its address, with the magic bytes written; closes it where nobody does. */
    private fun handOver(incoming: AcceptedSocket) {
        val address = incoming.address.uppercase()
        val takers = synchronized(lock) { waiters.filter { address in it.addresses } }
        if (takers.isEmpty()) {
            // Not a car the app waits for: let go at once, for whatever opened it.
            incoming.socket.closeQuietly()
            return
        }
        try {
            incoming.socket.output.write(FrameCodec.MAGIC)
            incoming.socket.output.flush()
        } catch (e: IOException) {
            // A link that cannot take them is a try that failed.
            incoming.socket.closeQuietly()
            return
        }
        val link = SerialLink(incoming.socket)
        // One that gave up meanwhile does not take it; the next does.
        if (takers.none { it.result.complete(AcceptedLink(incoming.address, link)) }) link.close()
    }
}

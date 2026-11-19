// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.TcpEndpoint
import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream

/**
 * A reliable, ordered stream of bytes to a head unit: Bluetooth serial, TCP, or a pipe in a test. Framing,
 * messages and the client sit above this and never learn which it is.
 */
interface Link : Closeable {
    /** What carries the link, for logs and the screen: `tcp`, `serial`, `local`. */
    val kind: String

    /** Reads block until bytes arrive, the other side closes (-1), or [close] makes them fail. */
    val input: InputStream

    val output: OutputStream

    /**
     * Whether the magic bytes have gone out on this link already, written by whoever opened it. The [Peer] that
     * dialled then sends none in front of its first frame. A serial link's have: see [BluetoothSerialLinkConnector].
     */
    val magicSent: Boolean get() = false

    /** Ends the link. A read or a write blocked on it fails. Safe to call twice, and from any thread. */
    override fun close()

    companion object {
        /** The [kind] of a Bluetooth serial link, which gets smaller frames and a keep-alive: see [AgentSession]. */
        const val SERIAL = "serial"
    }
}

/** Opens links. Which endpoints it can reach depends on the build: see the `di` package. */
interface LinkConnector {
    /** Whether this build has a way to reach [endpoint] at all. It depends on the kind of endpoint only, not its address. */
    fun supports(endpoint: Endpoint): Boolean

    /**
     * Opens a link to [endpoint]. The phone has dialled, so it speaks first.
     *
     * @throws IOException if the endpoint cannot be reached, or this build has no transport for it.
     */
    suspend fun connect(endpoint: Endpoint): Link

    /**
     * [connect], to a head unit whose agent [dials] or does not: one that has the car open the link to the phone, as
     * its last `hello` said ([com.nbtapps.store.model.HeadUnit.serialDial]). Only Bluetooth serial makes anything of
     * it: see [BluetoothSerialLinkConnector].
     */
    suspend fun connect(endpoint: Endpoint, dials: Boolean): Link = connect(endpoint)

    /** Whether [accept] can bring a link that the head unit at [endpoint] opens to the phone. Only Bluetooth serial can. */
    fun accepts(endpoint: Endpoint): Boolean = false

    /**
     * Waits for one of [endpoints] to open a link to the phone, as an agent that dials has the car do, and returns it
     * with the endpoint it came from: a link like one [connect] opens, the phone's magic bytes written. A caller that
     * is cancelled leaves nothing open.
     *
     * @throws IOException if the phone cannot listen now, or none of [endpoints] is one this connector [accepts].
     */
    suspend fun accept(endpoints: Collection<Endpoint>): IncomingLink = throw IOException("This version of the app can't take a link the head unit opens")
}

/** A link a head unit opened to the phone: see [LinkConnector.accept]. */
class IncomingLink(val endpoint: Endpoint, val link: Link)

/**
 * Whether this build reaches a head unit at a network address at all. Only the debug build does, and only the
 * test bench: the screens ask for an address where this is true and nowhere else.
 */
val LinkConnector.reachesTcp: Boolean
    get() = supports(TcpEndpoint("127.0.0.1", 1))

/** The connectors of a build as one: an endpoint goes to the first of them that reaches its kind. */
class LinkConnectors(private val connectors: List<LinkConnector>) : LinkConnector {
    override fun supports(endpoint: Endpoint): Boolean = connectors.any { it.supports(endpoint) }

    override suspend fun connect(endpoint: Endpoint): Link = connectorFor(endpoint).connect(endpoint)

    override suspend fun connect(endpoint: Endpoint, dials: Boolean): Link = connectorFor(endpoint).connect(endpoint, dials)

    override fun accepts(endpoint: Endpoint): Boolean = connectors.any { it.accepts(endpoint) }

    /** Through the first connector that accepts any of [endpoints], for those it accepts. */
    override suspend fun accept(endpoints: Collection<Endpoint>): IncomingLink {
        val taker = connectors.firstOrNull { connector -> endpoints.any(connector::accepts) } ?: return super.accept(endpoints)
        return taker.accept(endpoints.filter(taker::accepts))
    }

    private fun connectorFor(endpoint: Endpoint): LinkConnector =
        connectors.firstOrNull { it.supports(endpoint) } ?: throw IOException("This version of the app has no way to reach this head unit yet")
}

/** The link ended, or was never usable. The message is shown to the user. */
class LinkClosedException(message: String) : IOException(message)

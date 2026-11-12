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
}

/**
 * Whether this build reaches a head unit at a network address at all. Only the debug build does, and only the
 * test bench: the screens ask for an address where this is true and nowhere else.
 */
val LinkConnector.reachesTcp: Boolean
    get() = supports(TcpEndpoint("127.0.0.1", 1))

/** The connectors of a build as one: an endpoint goes to the first of them that reaches its kind. */
class LinkConnectors(private val connectors: List<LinkConnector>) : LinkConnector {
    override fun supports(endpoint: Endpoint): Boolean = connectors.any { it.supports(endpoint) }

    override suspend fun connect(endpoint: Endpoint): Link =
        (connectors.firstOrNull { it.supports(endpoint) } ?: throw IOException("This version of the app has no way to reach this head unit yet"))
            .connect(endpoint)
}

/** The link ended, or was never usable. The message is shown to the user. */
class LinkClosedException(message: String) : IOException(message)

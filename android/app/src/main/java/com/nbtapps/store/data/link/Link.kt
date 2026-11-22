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

    /**
     * Whether the head unit opened this link to the phone, as the car does for an agent that dials, rather than the
     * phone: see [LinkConnector.accept]. False for every link the phone opened.
     */
    val openedByCar: Boolean get() = false

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

    /**
     * [connect], where with [listenOnly] the phone opens no link itself and only waits for the one the head unit opens:
     * for an agent that said a link this phone opens is left to another program there
     * ([com.nbtapps.store.model.HeadUnit.phoneOpenedLeft]). Only Bluetooth serial makes anything of it: see
     * [BluetoothSerialLinkConnector]. A wait that brings no link fails with [NoCarLinkException].
     */
    suspend fun connect(endpoint: Endpoint, dials: Boolean, listenOnly: Boolean): Link = connect(endpoint, dials)

    /**
     * Knocks at the head unit at [endpoint], whose agent lists [ProtocolConstants.CAPABILITY_SERIAL_KNOCK]
     * ([com.nbtapps.store.model.HeadUnit.serialKnock]), and returns the link its car then opens to the phone: no link of
     * the phone's own is made. [bursts] is how many bursts of knocks are made at most, or null for the connector's own
     * number. Only Bluetooth serial knocks: see [BluetoothSerialLinkConnector]. Any other connector connects as to an
     * agent that dials.
     *
     * @throws KnockUnansweredException if no link of the car's came after the knocks.
     * @throws NoCarLinkException if the phone cannot listen for it.
     */
    suspend fun knock(endpoint: Endpoint, bursts: Int? = null): Link = connect(endpoint, dials = true)

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

    override suspend fun connect(endpoint: Endpoint, dials: Boolean, listenOnly: Boolean): Link =
        connectorFor(endpoint).connect(endpoint, dials, listenOnly)

    override suspend fun knock(endpoint: Endpoint, bursts: Int?): Link = connectorFor(endpoint).knock(endpoint, bursts)

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

/**
 * A connect that only waited for the head unit's link ([LinkConnector.connect] with `listenOnly`), or knocked for it
 * ([LinkConnector.knock]), and had none. The message is shown to the user, who may then have the phone connect itself.
 */
open class NoCarLinkException(message: String, cause: Throwable? = null) : IOException(message, cause)

/**
 * A connect that knocked ([LinkConnector.knock]) and had no link from the car after all its knocks, or [carAway]: it
 * stopped after a connect of its that did not reach the car (no answer in its time, or a failure while the phone had
 * no connection to the car), as the car is off or out of range then. What the user is told is the caller's to say, as
 * it knows how the phone and the car stand: the message here says only what happened.
 */
class KnockUnansweredException(message: String, val carAway: Boolean = false) : NoCarLinkException(message)

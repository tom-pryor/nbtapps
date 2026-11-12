// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.LinkClosing
import java.io.IOException
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.ReceiveChannel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonObject

/**
 * One open link to a head unit's agent, with everything that runs on it: the peer that reads it, the client
 * that asks, the blob server that answers, and the job events as they arrive.
 *
 * A Bluetooth serial link ([Link.SERIAL]) differs in two ways, both set by the agent repo's
 * `spec/protocol/constants.txt`:
 *
 *  - `hello` offers frames of 4,096 bytes at most, so a message never waits long behind a DATA frame.
 *  - Once `hello` is done the link is kept alive: see [KeepAlive]. A car that has gone out of range is found
 *    that way, since a serial link does not always say when it has ended.
 *
 * The agent keeps to the same rule, and during a long part of an install step it serves no link. If that lasts
 * longer than the silence allowed, this side ends the link; the install finishes on the head unit all the same,
 * and `job.status` tells the outcome after a reconnect.
 *
 * How a link ends: the phone made it, and the phone ends it. Before the agent ends a link of its own accord it
 * sends the event `link.closing` with why ([LinkClosing]), and this side closes the link the moment that arrives,
 * with nothing more sent ([closeAs]). On Bluetooth serial that is more than good manners: a car's Bluetooth has
 * been seen to give no later link any bytes, until the head unit was restarted, after a phone went on holding a
 * link whose other end the agent had closed. The agent waits [ProtocolConstants.SERIAL_LINGER_MS] for it.
 */
class AgentSession(
    link: Link,
    scope: CoroutineScope,
    io: CoroutineDispatcher = Dispatchers.IO,
    /** A serial link's keep-alive times. A test shortens them. */
    private val pingAfterMs: Long = ProtocolConstants.SERIAL_PING_AFTER_MS,
    private val silenceMs: Long = ProtocolConstants.SERIAL_SILENCE_MS,
    /** A clock in milliseconds that only goes forward. */
    private val now: () -> Long = ::monotonicMillis,
) : PeerHandler {
    val kind: String = link.kind
    private val serial = kind == Link.SERIAL
    private val greeted = CompletableDeferred<Unit>()

    val peer = Peer(link, dialled = true, handler = this, io = io, now = now)
    val client = AgentClient(
        peer,
        offeredFrame = if (serial) ProtocolConstants.SERIAL_MAX_FRAME else FrameCodec.MAX_PAYLOAD,
        onHello = { greeted.complete(Unit) },
    )
    val blobs = BlobServer(scope, io)

    private val eventChannel = Channel<JobEvent>(Channel.UNLIMITED)

    /** Every job event the agent sends on this link, in order. Closed when the link ends. */
    val events: ReceiveChannel<JobEvent> get() = eventChannel

    private val keepAlive: Job? = if (serial) scope.launch { watch() } else null

    /** Completes when the link has ended, with the reason to show. */
    val ended: Deferred<String> = scope.async {
        val reason = peer.run()
        keepAlive?.cancel()
        eventChannel.close()
        reason
    }

    private val told = AtomicReference<String?>(null)

    /**
     * Why the agent is ending this link, as [closeAs] was given it: one of [LinkClosing]'s, or whatever else the
     * agent said. Null while the link is open, and where it ended some other way.
     */
    val closing: String? get() = told.get()

    fun close(reason: String) = peer.close(reason)

    /**
     * Ends the link because the agent is ending it, for [reason]: what `link.closing` does. The words for the
     * user are [AgentText.closing]'s. It is also for whoever learns in another way that the agent is going, as
     * from a job of the agent's own update that reaches `restarting` on an agent from before there was the event.
     */
    fun closeAs(reason: String) {
        told.compareAndSet(null, reason)
        peer.close(AgentText.closing(reason))
    }

    override suspend fun onRequest(peer: Peer, id: Long, method: String, params: JsonObject) {
        when (method) {
            "blob.fetch" -> blobs.fetch(peer, id, params)
            "ping" -> peer.respond(id, JsonObject(emptyMap()))
            else -> peer.respondError(id, "unsupported", "a phone answers blob.fetch and ping only")
        }
    }

    override suspend fun onEvent(peer: Peer, name: String, seq: Long?, params: JsonObject) {
        if (name == LinkClosing.EVENT) {
            // Here, on the coroutine that reads the link, so that nothing is read after it and no time passes: the
            // agent holds its end of a serial link open until this side is gone.
            closeAs(params["reason"].stringOrNull().orEmpty())
            return
        }
        JobEvent.parse(name, seq, params)?.let { eventChannel.trySend(it) }
    }

    /** Pings the link when it is quiet and ends it when it stays silent. It starts with `hello`, as the agent's does. */
    private suspend fun watch() = coroutineScope {
        greeted.await()
        val rule = KeepAlive(pingAfterMs, silenceMs, now())
        while (peer.isOpen) {
            when (val step = rule.step(now(), peer.heardAt)) {
                is KeepAlive.Step.Wait -> delay(step.millis)
                // Not waited for here: its answer shows as bytes heard, and a write that is stuck must not stop the count.
                KeepAlive.Step.Ping -> launch { ping() }
                is KeepAlive.Step.Close -> peer.close("The head unit stopped answering")
            }
        }
    }

    private suspend fun ping() {
        try {
            withTimeoutOrNull(silenceMs) { peer.request("ping") }
        } catch (e: RemoteException) {
            // An answer all the same.
        } catch (e: IOException) {
            // The link ended.
        }
    }
}

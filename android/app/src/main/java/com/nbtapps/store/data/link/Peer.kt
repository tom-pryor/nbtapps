// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.json.JsonProfileException
import com.nbtapps.store.data.json.StrictJson
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/** The other side answered a request with an error. [code] is from the protocol's fixed list. */
class RemoteException(val code: String, message: String) : Exception(message)

/**
 * A request's answer together with the bytes the two went as: the payload of the frame that carried each. `auth`
 * hashes those of `hello`, so they are kept as they travelled and not written out again.
 */
class Exchange(val result: JsonObject, val sent: ByteArray, val received: ByteArray)

/** What a [Peer] hands up: the requests and events the other side sends. Called on the peer's reading coroutine. */
interface PeerHandler {
    /**
     * A request. It must be answered exactly once with [Peer.respond] or [Peer.respondError], here or later;
     * reading waits while this runs, so anything long is started and left to run.
     */
    suspend fun onRequest(peer: Peer, id: Long, method: String, params: JsonObject)

    /** An event. [seq] is null where the event carries none. Unknown events are to be ignored. */
    suspend fun onEvent(peer: Peer, name: String, seq: Long?, params: JsonObject)
}

/**
 * One end of a link at the message layer: frames in and out, requests matched to their responses, and the
 * rules that close a link. It mirrors the agent's `agent/peer.cpp`.
 *
 *  - A request is `{"id","method","params"}`, a response `{"id","result"}` or `{"id","error":{"code","message"}}`,
 *    and an event `{"event","seq"?,"params"}`. Each side counts its own ids from 1.
 *  - A message outside the strict JSON profile, or one that is none of the three, closes the link. So does a
 *    framing error. There is no resync: the phone connects again.
 *  - The phone never fetches, so a DATA frame from the agent closes the link too.
 *
 * [run] reads until the link ends. Everything else may be called from any coroutine; frames go out whole and
 * in the order the calls were made.
 */
class Peer(
    private val link: Link,
    /** Whether this side dialled: it then sends the magic bytes, and otherwise expects them. */
    private val dialled: Boolean,
    private val handler: PeerHandler,
    private val io: CoroutineDispatcher = Dispatchers.IO,
    /** A clock in milliseconds that only goes forward; only differences are used. */
    private val now: () -> Long = ::monotonicMillis,
) {
    /** The largest payload either side sends on this link: 65,536 until `hello` lowers it. */
    @Volatile
    var maxFrame: Int = FrameCodec.MAX_PAYLOAD
        set(value) {
            field = value
            decoder.maxPayload = value
        }

    /** Why the link ended, or null while it is open. */
    @Volatile
    var closeReason: String? = null
        private set

    val isOpen: Boolean get() = closeReason == null

    /** When bytes last arrived, by the peer's clock: whatever they were, the other side was there to send them. */
    @Volatile
    var heardAt: Long = now()
        private set

    private val decoder = FrameDecoder()
    private val writeLock = Mutex()

    /**
     * Guarded by [writeLock]: the magic bytes go out in front of the first frame, whoever sends it, unless the
     * link's opener has sent them already.
     */
    private var opened = !dialled || link.magicSent
    private val nextId = AtomicLong(1)

    /** The answers waited for: each completes with the result and the payload it came in. */
    private val waiting = ConcurrentHashMap<Long, CompletableDeferred<Pair<JsonObject, ByteArray>>>()

    /** Reads and dispatches until the link ends, and returns why it ended. Call it once. */
    suspend fun run(): String = withContext(io) {
        try {
            val buffer = ByteArray(16 * 1024)
            var magicSeen = if (dialled) FrameCodec.MAGIC.size else 0
            while (isOpen) {
                val count = link.input.read(buffer)
                if (count < 0) {
                    close("The head unit closed the link")
                    break
                }
                if (count > 0) heardAt = now()
                var offset = 0
                while (magicSeen < FrameCodec.MAGIC.size && offset < count) {
                    if (buffer[offset] != FrameCodec.MAGIC[magicSeen]) throw FrameException("the link did not open with the magic bytes")
                    magicSeen++
                    offset++
                }
                decoder.feed(buffer, offset, count - offset)
                while (isOpen) {
                    val frame = decoder.next() ?: break
                    if (frame.type == FrameCodec.TYPE_DATA) throw FrameException("a DATA frame, which a phone never asks for")
                    handleMessage(frame.payload)
                }
            }
        } catch (e: FrameException) {
            close("The link broke: ${e.message}")
        } catch (e: IOException) {
            close("The link to the head unit was lost")
        } finally {
            close("The link was closed")
        }
        closeReason ?: "The link was closed"
    }

    /**
     * Sends a request and waits for its answer, however long that takes: `job.plan` answers only after the
     * agent has fetched manifests.
     *
     * @throws RemoteException if the other side answers with an error.
     * @throws LinkClosedException if the link ends first, or the request does not fit in a frame.
     */
    suspend fun request(method: String, params: JsonObject = JsonObject(emptyMap())): JsonObject = exchange(method, params).result

    /** [request], with the bytes the request and its answer went as. */
    suspend fun exchange(method: String, params: JsonObject = JsonObject(emptyMap())): Exchange {
        val id = nextId.getAndIncrement()
        val text = buildJsonObject {
            put("id", id)
            put("method", method)
            put("params", params)
        }
        val answer = CompletableDeferred<Pair<JsonObject, ByteArray>>()
        waiting[id] = answer
        try {
            closeReason?.let { throw LinkClosedException(it) }
            val sent = sendMessage(text)
            val (result, received) = answer.await()
            return Exchange(result, sent, received)
        } finally {
            waiting.remove(id)
        }
    }

    suspend fun respond(id: Long, result: JsonObject) {
        val text = buildJsonObject {
            put("id", id)
            put("result", result)
        }
        if (encode(text).size > maxFrame) {
            respondError(id, "internal", "the reply does not fit in a frame on this link")
        } else {
            sendMessage(text)
        }
    }

    suspend fun respondError(id: Long, code: String, message: String) {
        sendMessage(
            buildJsonObject {
                put("id", id)
                put("error", buildJsonObject {
                    put("code", code)
                    put("message", message)
                })
            },
        )
    }

    suspend fun event(name: String, params: JsonObject) {
        sendMessage(
            buildJsonObject {
                put("event", name)
                put("params", params)
            },
        )
    }

    /** Sends one DATA frame of [stream], and returns once the link has taken it. */
    suspend fun data(stream: Int, bytes: ByteArray, offset: Int, length: Int, last: Boolean) {
        require(length <= maxFrame) { "a DATA frame larger than the link's frame size" }
        send(FrameCodec.encode(FrameCodec.TYPE_DATA, if (last) FrameCodec.FLAG_LAST else 0, stream, bytes, offset, length))
    }

    /** Ends the link. Requests still waiting fail with [reason]; the first reason given is the one kept. */
    fun close(reason: String) {
        synchronized(this) {
            if (closeReason != null) return
            closeReason = reason
        }
        link.close()
        for (answer in waiting.values) answer.completeExceptionally(LinkClosedException(reason))
    }

    /** Sends [message] in one frame, and returns the payload it went as. */
    private suspend fun sendMessage(message: JsonObject): ByteArray {
        val payload = encode(message)
        if (payload.size > maxFrame) throw LinkClosedException("A message of ${payload.size} bytes doesn't fit in one frame on this link")
        send(FrameCodec.encode(FrameCodec.TYPE_MSG, 0, 0, payload))
        return payload
    }

    private suspend fun send(bytes: ByteArray) {
        writeLock.withLock {
            closeReason?.let { throw LinkClosedException(it) }
            // A frame goes out whole or the link is done: a caller that is cancelled must not leave half of one.
            withContext(io + NonCancellable) {
                try {
                    if (!opened) {
                        link.output.write(FrameCodec.MAGIC)
                        opened = true
                    }
                    link.output.write(bytes)
                    link.output.flush()
                } catch (e: IOException) {
                    close("The link to the head unit was lost")
                }
            }
            closeReason?.let { throw LinkClosedException(it) }
        }
    }

    private suspend fun handleMessage(payload: ByteArray) {
        val root = try {
            StrictJson.parseObject(payload)
        } catch (e: JsonProfileException) {
            throw FrameException("a message outside the JSON profile: ${e.message}")
        }
        val id = root["id"].integerOrNull()
        val method = root["method"]
        val event = root["event"]
        val params = root["params"]

        if (method != null) {
            // Without a usable id there is nothing to answer to.
            val name = method.stringOrNull()
            if (name == null || id == null || id < 1) throw FrameException("a request with no usable id or method")
            if (params != null && params !is JsonObject) {
                respondError(id, "bad-request", "params is not an object")
                return
            }
            handler.onRequest(this, id, name, params ?: JsonObject(emptyMap()))
            return
        }
        if (event != null) {
            val name = event.stringOrNull()
            if (name == null || (params != null && params !is JsonObject)) throw FrameException("an event with no usable name or params")
            handler.onEvent(this, name, root["seq"].integerOrNull()?.takeIf { it >= 0 }, params ?: JsonObject(emptyMap()))
            return
        }
        val result = root["result"]
        val error = root["error"]
        if (id != null && (result is JsonObject || error is JsonObject) && !(result != null && error != null)) {
            val answer = waiting[id] ?: return // an answer to nothing we asked: ignored
            if (result is JsonObject) {
                answer.complete(result to payload)
            } else {
                val code = (error as JsonObject)["code"].stringOrNull() ?: throw FrameException("an error with no code")
                answer.completeExceptionally(RemoteException(code, error["message"].stringOrNull().orEmpty()))
            }
            return
        }
        throw FrameException("a message that is not a request, a response or an event")
    }

    private fun encode(message: JsonObject): ByteArray = Json.encodeToString(JsonObject.serializer(), message).encodeToByteArray()
}

/** Milliseconds on a clock that is never set back. */
internal fun monotonicMillis(): Long = System.nanoTime() / 1_000_000

internal fun JsonElement?.integerOrNull(): Long? = (this as? JsonPrimitive)?.takeIf { !it.isString }?.longOrNull

internal fun JsonElement?.stringOrNull(): String? = (this as? JsonPrimitive)?.takeIf { it.isString }?.content

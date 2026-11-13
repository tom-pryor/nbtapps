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

/**
 * Where the DATA frames of a stream the phone asked for go: one that the agent sends, as for `files.fetch`.
 * Called on the peer's reading coroutine, in the order the frames came, so a sink that takes long holds up the
 * link: it hands the bytes on and returns.
 */
interface StreamSink {
    /** One frame's payload. With [last] the stream is over, and nothing more comes on it. */
    fun onData(bytes: ByteArray, last: Boolean)

    /**
     * The stream ends without its last frame: the agent said why ([code] is from the protocol's list, as in
     * `files.error`), or with [code] null the link ended. Nothing more comes on it.
     */
    fun onEnd(code: String?, message: String)
}

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
 *  - A DATA frame from the agent is taken only for a stream the phone asked for with [requestStream], which
 *    the answer to that request names. Any other closes the link, as one after a stream's last frame does.
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

    /** By request id: where the stream that the answer to [requestStream] names is to go. */
    private val streamRequests = ConcurrentHashMap<Long, StreamSink>()

    /** The streams from the other side that are open, by their number. Only the reading coroutine adds or ends one. */
    private val streams = ConcurrentHashMap<Int, StreamSink>()

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
                    if (frame.type == FrameCodec.TYPE_DATA) {
                        val sink = streams[frame.stream] ?: throw FrameException("a DATA frame of stream ${frame.stream}, which the phone did not ask for")
                        if (frame.last) streams.remove(frame.stream)
                        sink.onData(frame.payload, frame.last)
                        continue
                    }
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

    /**
     * Sends a request whose answer may name a `stream` that the other side then sends, and has that stream's DATA
     * frames go to [sink]. It is taken up as the answer is read, before the next frame is, so not a frame of it is
     * missed however soon it follows. An answer that names no stream opens none, and [sink] is told nothing.
     *
     * The stream ends with its last frame, with [endStream], or with the link; [sink] hears of each but the second.
     */
    suspend fun requestStream(method: String, params: JsonObject, sink: StreamSink): JsonObject = exchange(method, params, sink).result

    /**
     * Ends the stream [stream] on this side: nothing more of it is taken, and a DATA frame of it that still comes
     * closes the link. For when the other side has said it sends no more, as the answer to `files.cancel` does.
     */
    fun endStream(stream: Int) {
        streams.remove(stream)
    }

    /** The other side says the stream [stream] cannot go on, as `files.error` does: its sink is told, and it ends. */
    fun failStream(stream: Int, code: String, message: String) {
        streams.remove(stream)?.onEnd(code, message)
    }

    /** [request], with the bytes the request and its answer went as. */
    suspend fun exchange(method: String, params: JsonObject = JsonObject(emptyMap())): Exchange = exchange(method, params, null)

    private suspend fun exchange(method: String, params: JsonObject, sink: StreamSink?): Exchange {
        val id = nextId.getAndIncrement()
        val text = buildJsonObject {
            put("id", id)
            put("method", method)
            put("params", params)
        }
        val answer = CompletableDeferred<Pair<JsonObject, ByteArray>>()
        waiting[id] = answer
        sink?.let { streamRequests[id] = it }
        try {
            closeReason?.let { throw LinkClosedException(it) }
            val sent = sendMessage(text)
            val (result, received) = answer.await()
            return Exchange(result, sent, received)
        } finally {
            waiting.remove(id)
            streamRequests.remove(id)
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
        for (stream in streams.keys.toList()) streams.remove(stream)?.onEnd(null, reason)
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
            val sent = withContext(io + NonCancellable) {
                try {
                    if (!opened) {
                        link.output.write(FrameCodec.MAGIC)
                        opened = true
                    }
                    link.output.write(bytes)
                    link.output.flush()
                    true
                } catch (e: IOException) {
                    close("The link to the head unit was lost")
                    false
                }
            }
            // Only a frame that did not go out fails here. The other side may answer one that did and end the link
            // straight after, as the agent does after system.restart, and the reader can have taken both in before
            // this line: the answer is then the request's, and the link's end is for whatever comes next.
            if (!sent) throw LinkClosedException(closeReason ?: "The link to the head unit was lost")
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
                // Here, before the next frame is read: the stream's first DATA frame may be that one.
                streamRequests.remove(id)?.let { sink -> openStream(result, sink) }
                answer.complete(result to payload)
            } else {
                val code = (error as JsonObject)["code"].stringOrNull() ?: throw FrameException("an error with no code")
                answer.completeExceptionally(RemoteException(code, error["message"].stringOrNull().orEmpty()))
            }
            return
        }
        throw FrameException("a message that is not a request, a response or an event")
    }

    /** Takes up the stream that [result] names, if any, for [sink]. */
    private fun openStream(result: JsonObject, sink: StreamSink) {
        val stream = result["stream"]?.integerOrNull() ?: return
        if (stream !in 1..MAX_STREAM) throw FrameException("an answer that names stream $stream, which is not a stream number")
        if (streams.putIfAbsent(stream.toInt(), sink) != null) throw FrameException("an answer that names stream $stream, which is open already")
        // The link may have closed meanwhile, and told only the sinks it found.
        closeReason?.let { reason -> streams.remove(stream.toInt())?.onEnd(null, reason) }
    }

    private fun encode(message: JsonObject): ByteArray = Json.encodeToString(JsonObject.serializer(), message).encodeToByteArray()
}

/** The highest stream number: streams are numbered from 1, in the frame header's 16 bits. */
private const val MAX_STREAM = 65_535L

/** Milliseconds on a clock that is never set back. */
internal fun monotonicMillis(): Long = System.nanoTime() / 1_000_000

internal fun JsonElement?.integerOrNull(): Long? = (this as? JsonPrimitive)?.takeIf { !it.isString }?.longOrNull

internal fun JsonElement?.stringOrNull(): String? = (this as? JsonPrimitive)?.takeIf { it.isString }?.content

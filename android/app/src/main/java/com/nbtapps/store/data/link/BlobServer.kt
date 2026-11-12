// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.RandomAccessFile
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.min
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/** Something the agent may fetch: an archive, named by its SHA-256, or a manifest, named by its own. */
sealed interface Blob {
    val size: Long

    /** An archive in the phone's package store. */
    class OfFile(val file: File, override val size: Long) : Blob

    /** A manifest, held in memory. */
    class OfBytes(val bytes: ByteArray) : Blob {
        override val size: Long get() = bytes.size.toLong()
    }
}

/**
 * Answers the agent's `blob.fetch` from the archives the phone holds. The agent drives: it asks for a range of
 * a blob by hash, the reply names a stream and its byte count, and DATA frames follow, the last one flagged.
 *
 *  - Only the blobs of the offers in the latest `job.plan`, and of a job that is fetching or paused, are
 *    served. Any other hash gets `not-found`.
 *  - One stream is open at a time; a fetch sent while one is open gets `busy`.
 *  - A stream that cannot go on ends with a `blob.error` event instead of a last frame.
 *
 * Nothing here is trusted by the agent: it checks every archive's hash and signature itself.
 */
class BlobServer(
    /** Streams run here, so they end with the link's owner. */
    private val scope: CoroutineScope,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) {
    private class Stream(val number: Int) {
        /** Set before the stream becomes the open one, so whoever finds it there can stop it. */
        lateinit var job: Job
    }

    @Volatile
    private var planned: Map<String, Blob> = emptyMap()

    @Volatile
    private var running: Map<String, Blob> = emptyMap()

    private val open = AtomicReference<Stream?>(null)
    private var nextStream = 1

    /** The blobs of the offers just sent in a `job.plan`, by hash. They replace those of the plan before. */
    fun offer(blobs: Map<String, Blob>) {
        planned = blobs
    }

    /** The latest plan has been started: its blobs stay served until [jobEnded], whatever is planned meanwhile. */
    fun jobStarted() {
        running = planned
    }

    fun jobEnded() {
        running = emptyMap()
    }

    /** Handles one `blob.fetch` request: answers it and, if it can be served, starts its stream. */
    suspend fun fetch(peer: Peer, id: Long, params: JsonObject) {
        val hash = params["sha256"].stringOrNull()
        val offset = params["offset"].integerOrNull()
        val length = params["length"].integerOrNull()
        if (hash == null || offset == null || offset < 0 || ("length" in params && (length == null || length < 1))) {
            peer.respondError(id, "bad-request", "blob.fetch takes sha256, offset and an optional length")
            return
        }
        if (open.get() != null) {
            peer.respondError(id, "busy", "a stream is open")
            return
        }
        val blob = running[hash] ?: planned[hash]
        if (blob == null) {
            peer.respondError(id, "not-found", "no such blob here")
            return
        }
        val count = length ?: (blob.size - offset)
        if (offset >= blob.size || count > blob.size - offset) {
            peer.respondError(id, "out-of-range", "the range is not inside the blob")
            return
        }
        if (blob is Blob.OfFile && blob.file.length() != blob.size) {
            peer.respondError(id, "not-found", "the file has gone")
            return
        }

        val stream = Stream(nextStream)
        nextStream = if (nextStream == 65535) 1 else nextStream + 1
        stream.job = scope.launch(io, start = CoroutineStart.LAZY) { send(peer, stream, blob, offset, count) }
        open.set(stream)
        try {
            // The reply goes out before the first DATA frame, because the stream only starts after it.
            peer.respond(id, buildJsonObject {
                put("stream", stream.number)
                put("size", count)
            })
        } catch (e: LinkClosedException) {
            open.compareAndSet(stream, null)
            stream.job.cancel()
            throw e
        }
        stream.job.start()
    }

    /**
     * Ends the open stream because the user stopped the job: no more DATA frames follow, and the agent is told
     * with `blob.error`. Returns false if no stream was open. The caller sends `job.cancel` next.
     */
    suspend fun cancel(peer: Peer): Boolean {
        val stream = open.getAndSet(null) ?: return false
        stream.job.cancelAndJoin()
        try {
            peer.event("blob.error", error(stream, "cancelled", "stopped on the phone"))
        } catch (e: LinkClosedException) {
            // Nothing left to tell: the agent pauses a job whose link is gone.
        }
        return true
    }

    private suspend fun send(peer: Peer, stream: Stream, blob: Blob, offset: Long, count: Long) {
        val chunk = ByteArray(peer.maxFrame)
        var left = count
        try {
            when (blob) {
                is Blob.OfBytes -> {
                    var at = offset.toInt()
                    while (left > 0) {
                        val n = min(left, chunk.size.toLong()).toInt()
                        if (!frame(peer, stream, blob.bytes, at, n, last = left == n.toLong())) return
                        at += n
                        left -= n
                    }
                }
                is Blob.OfFile -> RandomAccessFile(blob.file, "r").use { file ->
                    file.seek(offset)
                    while (left > 0) {
                        val n = min(left, chunk.size.toLong()).toInt()
                        file.readFully(chunk, 0, n)
                        if (!frame(peer, stream, chunk, 0, n, last = left == n.toLong())) return
                        left -= n
                    }
                }
            }
        } catch (e: LinkClosedException) {
            open.compareAndSet(stream, null)
        } catch (e: IOException) {
            // The archive could not be read. The agent keeps the bytes it has and decides whether to ask again.
            if (open.compareAndSet(stream, null)) {
                val code = if (e is FileNotFoundException) "not-found" else "io-error"
                try {
                    peer.event("blob.error", error(stream, code, "the archive could not be read on the phone"))
                } catch (closed: LinkClosedException) {
                    // The link went as well.
                }
            }
        }
    }

    /**
     * Sends one DATA frame unless the stream has been taken away by [cancel]. The stream stops being the open
     * one before its last frame goes out, because the agent may ask for the next range the moment it has it.
     */
    private suspend fun frame(peer: Peer, stream: Stream, bytes: ByteArray, offset: Int, length: Int, last: Boolean): Boolean {
        if (last) {
            if (!open.compareAndSet(stream, null)) return false
        } else if (open.get() !== stream) {
            return false
        }
        peer.data(stream.number, bytes, offset, length, last)
        return true
    }

    private fun error(stream: Stream, code: String, message: String) = buildJsonObject {
        put("stream", stream.number)
        put("code", code)
        put("message", message)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.logs

import com.nbtapps.store.data.link.AgentClient
import com.nbtapps.store.data.link.RemoteException
import com.nbtapps.store.data.link.StreamSink
import com.nbtapps.store.model.AgentFile
import com.nbtapps.store.model.FileCompress
import java.io.File
import java.io.IOException
import java.io.OutputStream
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.zip.DataFormatException
import java.util.zip.Inflater
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.withContext

/** What a pull of a package's logs is to get: the files the user chose of those `files.list` gave. */
data class LogPullRequest(
    val packageId: String,
    val files: List<AgentFile>,
    /** Only the last this many bytes of each file, for files that are larger. Null for the whole of each. */
    val tailBytes: Long? = null,
    /** The head unit listed more files than it could send in one answer, and the oldest are not among [files]. */
    val listingCut: Boolean = false,
)

/** What a pull's `info.txt` says of where the files came from. Nothing in it is the user's but the head unit's name. */
data class LogPullAbout(
    val packageName: String,
    /** The installed version, or null where the head unit's list does not have the package, as for the agent's own logs. */
    val packageVersion: String?,
    val headUnitName: String,
    val agentVersion: String,
    val appVersion: String,
    /** When the pull began, in milliseconds since 1970 by the phone's clock. */
    val pulledAt: Long,
)

/** How one file of a pull came out. */
data class PulledFile(
    val file: AgentFile,
    /** Its name in the ZIP. Null for a file that is not in it. */
    val entry: String?,
    /** The file's bytes that were asked for and came: all of it, or with a tail its last part. */
    val bytes: Long = 0,
    /** From which byte of the file they are: above 0 where only its last part was asked for. */
    val offset: Long = 0,
    /** The bytes that crossed the link, compressed. */
    val sent: Long = 0,
    /** Why the file is not in the ZIP, for the user. Null for one that is. */
    val failure: String? = null,
)

/** Where a pull of logs stands. */
sealed interface LogPullState {
    val packageId: String

    data class Pulling(
        override val packageId: String,
        /** The file being fetched, counted from 1, and how many there are. */
        val file: Int,
        val files: Int,
        /** Bytes of the files that have come, before compression, and how many are expected in all. */
        val bytes: Long,
        val total: Long,
    ) : LogPullState

    data class Done(override val packageId: String, val zip: File, val files: List<PulledFile>) : LogPullState {
        val included: Int get() = files.count { it.entry != null }
    }

    data class Failed(override val packageId: String, val reason: String) : LogPullState

    data class Cancelled(override val packageId: String) : LogPullState
}

/**
 * Pulls log files of one package from the agent over [client] and writes them into a ZIP, with an `info.txt`
 * that says where they came from and how each came out: what the user hands on with Android's share sheet.
 *
 * The files are fetched one after another, each as a raw deflate stream that is inflated as it comes. Each is
 * checked: the bytes it inflates to are as many as the agent's answer said, and its deflate stream ends with the
 * stream's last frame. A file that fails the check, or that the agent cannot read, is left out and written up in
 * `info.txt`, and the pull goes on with the next; a file is never in the ZIP cut short. The link ending ends the
 * pull ([IOException]). A pull whose caller is cancelled stops the stream that is open with `files.cancel`.
 */
class LogPull(
    private val client: AgentClient,
    /** Where the files are inflated to before they go into the ZIP. Emptied again by the pull. */
    private val workDir: File,
) {
    /**
     * Pulls [request] into [zip], and returns how each file came out. [onProgress] is told the file being
     * fetched, counted from 1, and the bytes come so far of how many are expected in all.
     *
     * @throws RemoteException where the agent refuses a fetch as a whole, as with `busy` while a job runs.
     * @throws IOException where the link ends, or the ZIP cannot be written.
     */
    suspend fun run(request: LogPullRequest, about: LogPullAbout, zip: File, onProgress: (file: Int, bytes: Long, total: Long) -> Unit = { _, _, _ -> }): List<PulledFile> {
        val total = request.files.sumOf { wanted(it, request.tailBytes) }
        var done = 0L
        val results = ArrayList<PulledFile>()
        val names = HashSet<String>()
        workDir.mkdirs()
        val part = File(workDir, "file.part")
        try {
            ZipOutputStream(zip.outputStream().buffered()).use { out ->
                for ((index, file) in request.files.withIndex()) {
                    onProgress(index + 1, done, total)
                    val base = done
                    val came = fetch(request.packageId, file, request.tailBytes, part) { bytes -> onProgress(index + 1, base + bytes, total) }
                    done = base + wanted(file, request.tailBytes)
                    if (came.failure != null) {
                        results += came
                        continue
                    }
                    val entry = unique(entryName(request.packageId, file.path), names)
                    out.putNextEntry(ZipEntry(entry).apply { if (file.modified > 0) time = file.modified * 1000 })
                    part.inputStream().use { it.copyTo(out) }
                    out.closeEntry()
                    results += came.copy(entry = entry)
                }
                out.putNextEntry(ZipEntry(unique("info.txt", names)).apply { time = about.pulledAt })
                out.write(info(request, about, results).encodeToByteArray())
                out.closeEntry()
            }
        } catch (e: Throwable) {
            zip.delete()
            throw e
        } finally {
            part.delete()
        }
        return results
    }

    /** One stream's bytes, as the peer hands them on: [Chunk.End] where it ends without its last frame. */
    private sealed interface Chunk {
        class Data(val bytes: ByteArray, val last: Boolean) : Chunk
        class End(val code: String?, val message: String) : Chunk
    }

    /** Hands a stream's frames from the link's reading coroutine to the one that inflates them. */
    private class Sink : StreamSink {
        val chunks = Channel<Chunk>(Channel.UNLIMITED)
        override fun onData(bytes: ByteArray, last: Boolean) {
            chunks.trySend(Chunk.Data(bytes, last))
        }

        override fun onEnd(code: String?, message: String) {
            chunks.trySend(Chunk.End(code, message))
        }
    }

    /** Fetches [file] into [part], inflated, and says how it came out. [onBytes] is told the bytes written so far. */
    private suspend fun fetch(packageId: String, file: AgentFile, tailBytes: Long?, part: File, onBytes: (Long) -> Unit): PulledFile {
        val offset = tailBytes?.let { (file.size - it).coerceAtLeast(0) } ?: 0
        val sink = Sink()
        val fetch = try {
            client.filesFetch(packageId, file.path, FileCompress.DEFLATE, sink, offset = offset.takeIf { it > 0 })
        } catch (e: RemoteException) {
            // A file that went or shrank since it was listed. Anything else is about the pull as a whole.
            when (e.code) {
                "not-found" -> return PulledFile(file, null, failure = "It is no longer on the head unit")
                "out-of-range" -> return PulledFile(file, null, failure = "It became shorter after it was listed")
                else -> throw e
            }
        }
        // An empty file: nothing follows, and it goes into the ZIP as it is.
        val stream = fetch.stream ?: return PulledFile(file, null, offset = offset).also { part.writeBytes(ByteArray(0)) }
        return part.outputStream().use { out -> receive(file, offset, stream, fetch.size, sink, out, onBytes) }
    }

    /** Inflates the stream [stream] of [file], [size] bytes before compression, from [sink] into [out]. */
    private suspend fun receive(file: AgentFile, offset: Long, stream: Int, size: Long, sink: Sink, out: OutputStream, onBytes: (Long) -> Unit): PulledFile {
        var open = true
        val inflater = Inflater(true)
        val into = Inflating(inflater, out, onBytes)
        fun failed(why: String) = PulledFile(file, null, offset = offset, sent = into.sent, failure = why)
        try {
            while (true) {
                when (val chunk = sink.chunks.receive()) {
                    is Chunk.End -> {
                        open = false
                        // The link ended: there is no next file to fetch either.
                        if (chunk.code == null) throw IOException(chunk.message.ifEmpty { "The link to the head unit was lost" })
                        return failed("The head unit couldn't read it to the end")
                    }
                    is Chunk.Data -> {
                        if (chunk.last) open = false
                        val damaged = into.take(chunk.bytes, size)
                            ?: if (chunk.last && !inflater.finished()) "its compressed data stopped before its end" else null
                        if (damaged != null) return failed("It came damaged: $damaged")
                        if (chunk.last) {
                            if (into.written != size) return failed("It came damaged: ${into.written} bytes of $size")
                            return PulledFile(file, null, bytes = into.written, offset = offset, sent = into.sent)
                        }
                    }
                }
            }
        } finally {
            inflater.end()
            // A stream left before its end, by a damaged file or by the caller going: the agent stops it. What it
            // had queued meanwhile goes to the sink, which nobody reads any more.
            if (open) withContext(NonCancellable) { stop(stream) }
        }
    }

    private suspend fun stop(stream: Int) {
        try {
            client.filesCancel(stream)
        } catch (e: RemoteException) {
            // Then it ends by itself.
        } catch (e: IOException) {
            // The link ended, and the stream with it.
        }
    }

    /** One file's deflate stream, inflated into [out] frame by frame. */
    private class Inflating(private val inflater: Inflater, private val out: OutputStream, private val onBytes: (Long) -> Unit) {
        private val buffer = ByteArray(64 * 1024)
        var written = 0L
            private set
        var sent = 0L
            private set

        /** Inflates [bytes], one frame's. Returns what is wrong with the data, or null if nothing is yet. */
        fun take(bytes: ByteArray, size: Long): String? {
            sent += bytes.size
            if (bytes.isEmpty()) return null
            if (inflater.finished()) return "there is more after its end"
            inflater.setInput(bytes)
            try {
                while (true) {
                    val count = inflater.inflate(buffer)
                    if (count > 0) {
                        written += count
                        if (written > size) return "it is longer than the head unit said"
                        out.write(buffer, 0, count)
                    }
                    if (inflater.finished()) {
                        onBytes(written)
                        return if (inflater.remaining > 0) "there is more after its end" else null
                    }
                    if (inflater.needsDictionary()) return "it isn't the data it should be"
                    if (count == 0 && inflater.needsInput()) break
                }
            } catch (e: DataFormatException) {
                return "it isn't the data it should be"
            }
            onBytes(written)
            return null
        }
    }

    /** What `info.txt` says. */
    private fun info(request: LogPullRequest, about: LogPullAbout, results: List<PulledFile>): String = buildString {
        val version = about.packageVersion?.let { " $it" }.orEmpty()
        appendLine("Logs of ${about.packageName} (${request.packageId}$version)")
        appendLine("From the head unit \"${about.headUnitName}\", agent ${about.agentVersion}")
        appendLine("Pulled ${time(about.pulledAt / 1000)} by the phone's clock, with NBTApps ${about.appVersion}")
        appendLine()
        appendLine("Each file's time is the head unit's clock, which may be wrong.")
        request.tailBytes?.let { appendLine("Of a file larger than $it bytes, only its last $it bytes were asked for.") }
        appendLine()
        val included = results.filter { it.entry != null }
        if (included.isNotEmpty()) appendLine("In this file:")
        for (one in included) {
            val part = if (one.offset > 0) "its last ${one.bytes} bytes of ${one.offset + one.bytes}" else "${one.bytes} bytes"
            appendLine("  ${one.entry}")
            appendLine("    ${one.file.path}: $part, changed ${time(one.file.modified)}, ${one.sent} bytes over the link")
        }
        val missing = results.filter { it.entry == null }
        if (missing.isNotEmpty() || request.listingCut) {
            appendLine()
            appendLine("Not in this file:")
            for (one in missing) appendLine("  ${one.file.path}: ${one.failure}")
            if (request.listingCut) appendLine("  Older files: the head unit's list of them was too long to send in one answer.")
        }
    }

    companion object {
        /** The file's bytes a pull asks for: the whole file, or with [tailBytes] at most that many. */
        fun wanted(file: AgentFile, tailBytes: Long?): Long = if (tailBytes == null) file.size else minOf(file.size, tailBytes)

        /**
         * The name in the ZIP for the file [path] of the package [packageId]: a folder of the package's, then the
         * path with its variable as a folder of its own (`${data}/log/a.log` is `data/log/a.log`) and an absolute
         * path under `system/`. Whatever the agent sent, the name stays inside that folder: whoever opens the ZIP
         * gets no file outside where they open it.
         */
        fun entryName(packageId: String, path: String): String {
            val variable = Regex("""^\$\{([a-z]+)\}/""").find(path)
            val rest = when {
                variable != null -> variable.groupValues[1] + "/" + path.substring(variable.value.length)
                path.startsWith("/") -> "system$path"
                else -> path
            }
            val parts = (listOf(packageId) + rest.split('/')).map(::safe).filter { it.isNotEmpty() && it != "." && it != ".." }
            return parts.joinToString("/")
        }

        private fun safe(part: String): String = part.map { c -> if (c < ' ' || c == '\\' || c == ':' || c == '\u007f') '_' else c }.joinToString("")

        /** [name], or where it is taken already, the same with a number before its extension. */
        private fun unique(name: String, taken: MutableSet<String>): String {
            var candidate = name
            var n = 2
            while (!taken.add(candidate)) {
                val dot = name.lastIndexOf('.').takeIf { it > name.lastIndexOf('/') + 1 } ?: name.length
                candidate = name.substring(0, dot) + "-$n" + name.substring(dot)
                n++
            }
            return candidate
        }

        private fun time(seconds: Long): String =
            if (seconds <= 0) "at a time not known" else DateTimeFormatter.ISO_INSTANT.format(Instant.ofEpochSecond(seconds).atOffset(ZoneOffset.UTC))
    }
}

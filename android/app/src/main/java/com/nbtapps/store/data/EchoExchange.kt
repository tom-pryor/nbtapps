// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import com.nbtapps.store.model.EchoReport
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread
import kotlin.math.min

/**
 * Sends a known byte pattern down [output] and checks it against what comes back on [input], for a
 * peer that returns every byte it receives.
 *
 * Works on plain streams so it can be tested without Bluetooth. Reads block: the caller bounds the
 * run by closing the streams, which makes the pending read fail.
 */
class EchoExchange(
    private val input: InputStream,
    private val output: OutputStream,
) {
    /**
     * Sends [totalBytes] and waits for all of them to return. [onProgress] gets the number of bytes
     * echoed so far.
     *
     * @throws IOException if the link closes or fails before everything has come back.
     */
    fun run(totalBytes: Int, onProgress: (received: Int) -> Unit = {}): EchoReport {
        require(totalBytes > 0) { "Nothing to send" }
        val start = System.nanoTime()
        val verifier = Verifier()

        // A short first message on its own measures the round trip.
        val pingBytes = min(PING_BYTES, totalBytes)
        output.write(pattern(0, pingBytes))
        output.flush()
        readEcho(pingBytes, totalBytes, verifier, onProgress)
        val roundTripMillis = elapsedMillis(start)

        // The rest is written from a second thread: writing and reading in turn would stall as soon
        // as both sides' buffers fill.
        val writeError = AtomicReference<IOException?>()
        val stopWriting = AtomicBoolean(false)
        val writer = thread(name = "echo-writer") {
            try {
                var offset = pingBytes
                while (offset < totalBytes && !stopWriting.get()) {
                    val length = min(CHUNK_BYTES, totalBytes - offset)
                    output.write(pattern(offset, length))
                    offset += length
                }
                output.flush()
            } catch (e: IOException) {
                writeError.set(e)
            }
        }
        try {
            readEcho(totalBytes, totalBytes, verifier, onProgress)
        } catch (e: IOException) {
            // A failed write is the cause when both sides failed.
            throw writeError.get() ?: e
        } finally {
            stopWriting.set(true)
            writer.join(WRITER_JOIN_MILLIS)
        }

        return EchoReport(
            bytesSent = totalBytes,
            bytesReceived = verifier.received,
            firstMismatchAt = verifier.firstMismatchAt,
            roundTripMillis = roundTripMillis,
            totalMillis = elapsedMillis(start),
        )
    }

    /** Reads until [upTo] bytes in total have come back. */
    private fun readEcho(upTo: Int, totalBytes: Int, verifier: Verifier, onProgress: (Int) -> Unit) {
        val buffer = ByteArray(READ_BYTES)
        while (verifier.received < upTo) {
            val count = try {
                input.read(buffer, 0, min(buffer.size, upTo - verifier.received))
            } catch (e: IOException) {
                throw IOException("Stopped after ${verifier.received} of $totalBytes bytes came back: ${e.message}", e)
            }
            if (count < 0) {
                throw IOException("The link closed after ${verifier.received} of $totalBytes bytes came back")
            }
            verifier.check(buffer, count)
            onProgress(verifier.received)
        }
    }

    private class Verifier {
        var received = 0
            private set
        var firstMismatchAt: Int? = null
            private set

        fun check(buffer: ByteArray, count: Int) {
            for (i in 0 until count) {
                if (firstMismatchAt == null && buffer[i] != patternByte(received + i)) {
                    firstMismatchAt = received + i
                }
            }
            received += count
        }
    }

    companion object {
        const val PING_BYTES = 16
        private const val CHUNK_BYTES = 4096
        private const val READ_BYTES = 16 * 1024
        private const val WRITER_JOIN_MILLIS = 1_000L

        /** The byte sent at [offset]. Depends on the offset alone, so lost or reordered data shows up. */
        fun patternByte(offset: Int): Byte = ((offset * 31 + offset / 251) and 0xFF).toByte()

        private fun pattern(offset: Int, length: Int) = ByteArray(length) { patternByte(offset + it) }

        private fun elapsedMillis(startNanos: Long) = (System.nanoTime() - startNanos) / 1_000_000
    }
}

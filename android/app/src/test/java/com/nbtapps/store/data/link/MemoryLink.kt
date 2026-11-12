// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Two ends of a link that lives in memory, for tests. It behaves as a socket does: closing one end makes that
 * end's reads and writes fail, and gives the other end the end of the stream once it has read what was sent.
 */
class MemoryLink private constructor(private val incoming: Pipe, private val outgoing: Pipe, override val kind: String) : Link {
    override val input: InputStream = incoming.input
    override val output: OutputStream = outgoing.output

    override fun close() {
        incoming.closeReader()
        outgoing.closeWriter()
    }

    private class Pipe(capacity: Int) {
        private val queue = LinkedBlockingQueue<ByteArray>(capacity)
        private var current = ByteArray(0)
        private var used = 0

        @Volatile
        private var readerClosed = false

        @Volatile
        private var writerClosed = false

        val input = object : InputStream() {
            override fun read(): Int {
                val one = ByteArray(1)
                return if (read(one, 0, 1) < 0) -1 else one[0].toInt() and 0xFF
            }

            override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
                if (length == 0) return 0
                while (used == current.size) {
                    if (readerClosed) throw IOException("the link is closed")
                    // A test that waits for bytes that never come fails here instead of hanging the build.
                    val next = queue.poll(READ_TIMEOUT_SECONDS, TimeUnit.SECONDS)
                        ?: throw IOException("nothing arrived on the test link in $READ_TIMEOUT_SECONDS seconds")
                    if (next === END) {
                        queue.offer(END) // so every later read ends too
                        if (readerClosed) throw IOException("the link is closed")
                        return -1
                    }
                    current = next
                    used = 0
                }
                val count = minOf(length, current.size - used)
                System.arraycopy(current, used, buffer, offset, count)
                used += count
                return count
            }
        }

        val output = object : OutputStream() {
            override fun write(byte: Int) = write(byteArrayOf(byte.toByte()), 0, 1)

            override fun write(bytes: ByteArray, offset: Int, length: Int) {
                val chunk = bytes.copyOfRange(offset, offset + length)
                // Waits for room in turns, so that closing either end also fails a write that is waiting.
                do {
                    if (writerClosed || readerClosed) throw IOException("the link is closed")
                } while (length > 0 && !queue.offer(chunk, WRITE_POLL_MILLIS, TimeUnit.MILLISECONDS))
            }
        }

        fun closeReader() {
            readerClosed = true
            queue.clear() // frees a write that is waiting for room, and wakes a read that is waiting
            queue.offer(END)
        }

        fun closeWriter() {
            writerClosed = true
            // A full pipe takes the mark once the reader has made room.
            if (!queue.offer(END)) Thread { queue.put(END) }.apply { isDaemon = true }.start()
        }
    }

    companion object {
        private val END = ByteArray(0)
        private const val READ_TIMEOUT_SECONDS = 10L
        private const val WRITE_POLL_MILLIS = 20L

        /**
         * The two ends of one link. With [capacity], each direction holds that many writes and no more, so a
         * side that does not read makes the other side's writes wait, as a slow link does. [kind] is what both
         * ends say they are: [Link.SERIAL] gets what a Bluetooth serial link gets.
         */
        fun pair(capacity: Int = Int.MAX_VALUE, kind: String = "memory"): Pair<MemoryLink, MemoryLink> {
            val there = Pipe(capacity)
            val back = Pipe(capacity)
            return MemoryLink(incoming = back, outgoing = there, kind) to MemoryLink(incoming = there, outgoing = back, kind)
        }
    }
}

/** Reads whole frames from a link with blocking calls: what a test's stand-in for the agent uses. */
class FrameReader(private val link: Link) {
    private val decoder = FrameDecoder()
    private val buffer = ByteArray(8192)

    /** The next frame, or null once the other side has closed the link. */
    fun next(): Frame? {
        while (true) {
            decoder.next()?.let { return it }
            val count = link.input.read(buffer)
            if (count < 0) return null
            decoder.feed(buffer, 0, count)
        }
    }

    /** Reads exactly [count] bytes that come before any frame, such as the magic bytes. */
    fun raw(count: Int): ByteArray {
        val out = ByteArray(count)
        var filled = 0
        while (filled < count) {
            val n = link.input.read(out, filled, count - filled)
            if (n < 0) throw IOException("the link closed after $filled of $count bytes")
            filled += n
        }
        return out
    }
}

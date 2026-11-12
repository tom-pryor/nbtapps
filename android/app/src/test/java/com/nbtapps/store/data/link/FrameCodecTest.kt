// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.crypto.Hex
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** The codec against `spec/protocol/frames.vectors` of the agent repo: the frames both sides must agree on. */
class FrameCodecTest {

    private class Good(val type: Int, val flags: Int, val stream: Int, val payload: ByteArray, val frame: ByteArray, val what: String)

    private val lines = AgentVectors.file("protocol/frames.vectors").readLines()

    private fun bytes(hex: String) = if (hex == "-") ByteArray(0) else AgentVectors.hex(hex)

    private val good: List<Good> = lines.mapIndexedNotNull { i, line ->
        val fields = line.split(' ')
        if (fields[0] != "frame") null
        else Good(fields[1].toInt(), fields[2].toInt(), fields[3].toInt(), bytes(fields[4]), bytes(fields[5]), lines[i - 1])
    }

    private val bad: List<Pair<String, ByteArray>> = lines.mapIndexedNotNull { i, line ->
        if (line.startsWith("bad ")) lines[i - 1] to bytes(line.substringAfter(' ')) else null
    }

    @Test
    fun `the vectors file is whole`() {
        assertTrue(good.size >= 7)
        assertTrue(bad.size >= 12)
        assertEquals("magic " + Hex.encode(FrameCodec.MAGIC), lines.first { it.startsWith("magic") })
    }

    @Test
    fun `every good frame encodes to the same bytes`() {
        for (frame in good) {
            assertArrayEquals(frame.what, frame.frame, FrameCodec.encode(frame.type, frame.flags, frame.stream, frame.payload))
        }
    }

    @Test
    fun `every good frame decodes, whole or fed a byte at a time`() {
        for (frame in good) {
            for (step in listOf(frame.frame.size, 1, 7)) {
                val decoder = FrameDecoder()
                var decoded: Frame? = null
                var at = 0
                while (at < frame.frame.size) {
                    // Cut off anywhere, a good frame is not an error: the decoder waits for more.
                    assertNull(frame.what, decoded)
                    val count = minOf(step, frame.frame.size - at)
                    decoder.feed(frame.frame, at, count)
                    at += count
                    decoded = decoder.next()
                }
                val out = requireNotNull(decoded) { frame.what }
                assertEquals(frame.what, frame.type, out.type)
                assertEquals(frame.what, frame.flags, out.flags)
                assertEquals(frame.what, frame.stream, out.stream)
                assertArrayEquals(frame.what, frame.payload, out.payload)
                assertEquals(frame.what, frame.flags and 2 != 0, out.last)
                assertNull(decoder.next())
            }
        }
    }

    @Test
    fun `good frames decode one after another from one stream of bytes`() {
        val decoder = FrameDecoder()
        val all = good.fold(ByteArray(0)) { bytes, frame -> bytes + frame.frame }
        var at = 0
        val out = ArrayList<Frame>()
        while (at < all.size) {
            val count = minOf(11, all.size - at)
            decoder.feed(all, at, count)
            at += count
            while (true) out += decoder.next() ?: break
        }
        assertEquals(good.map { it.payload.toList() }, out.map { it.payload.toList() })
    }

    @Test
    fun `every bad frame is refused, and the decoder stays failed`() {
        for ((what, bytes) in bad) {
            val decoder = FrameDecoder()
            decoder.feed(bytes, 0, bytes.size)
            try {
                val frame = decoder.next()
                fail("$what: " + if (frame == null) "the decoder waits for more" else "decoded")
            } catch (e: FrameException) {
                // refused, as it must be
            }
            val next = good.first().frame
            decoder.feed(next, 0, next.size)
            assertThrows(what, FrameException::class.java) { decoder.next() }
        }
    }

    @Test
    fun `a frame above the size agreed in hello is refused from its header alone`() {
        val decoder = FrameDecoder().apply { maxPayload = 1024 }
        val frame = FrameCodec.encode(FrameCodec.TYPE_DATA, 0, 1, ByteArray(1025))
        decoder.feed(frame, 0, FrameCodec.HEADER_BYTES)
        assertThrows(FrameException::class.java) { decoder.next() }

        val fits = FrameDecoder().apply { maxPayload = 1024 }
        val whole = FrameCodec.encode(FrameCodec.TYPE_DATA, 0, 1, ByteArray(1024))
        fits.feed(whole, 0, whole.size)
        assertEquals(1024, fits.next()?.payload?.size)
    }
}

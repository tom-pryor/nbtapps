// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import java.util.zip.CRC32

/** One frame of the phone and agent protocol. */
class Frame(val type: Int, val flags: Int, val stream: Int, val payload: ByteArray) {
    /** The last DATA frame of its stream. */
    val last: Boolean get() = flags and FrameCodec.FLAG_LAST != 0
}

/** The bytes are not a frame of this protocol. The link is closed over it; there is no resync. */
class FrameException(message: String) : Exception(message)

/**
 * Frames as the plan document's "Framing" section sets them. Integers are little-endian:
 *
 *     u8  type      1 = MSG (UTF-8 JSON), 2 = DATA (binary)
 *     u8  flags     bit 0: reserved for a compressed payload; bit 1: last DATA frame of its stream
 *     u16 stream    0 for MSG; for DATA, the stream its blob.fetch or files.fetch reply named
 *     u32 length    payload bytes, at most 65,536 or the lower maxFrame from hello
 *         payload
 *     u32 check     CRC32 over the header and the payload
 *
 * The agent's `agent/frame.cpp` is the other implementation, and `spec/protocol/frames.vectors` in the agent
 * repo holds the frames both must agree on.
 */
object FrameCodec {
    const val HEADER_BYTES = 8
    const val CHECK_BYTES = 4
    const val MAX_PAYLOAD = ProtocolConstants.FRAME_MAX_PAYLOAD
    const val MIN_PAYLOAD = ProtocolConstants.FRAME_MIN_PAYLOAD

    const val TYPE_MSG = 1
    const val TYPE_DATA = 2
    const val FLAG_COMPRESSED = 1
    const val FLAG_LAST = 2

    /** `NBTL`: what the side that dialled sends before its first frame. */
    val MAGIC: ByteArray get() = byteArrayOf(0x4E, 0x42, 0x54, 0x4C)

    fun encode(type: Int, flags: Int, stream: Int, payload: ByteArray, offset: Int = 0, length: Int = payload.size): ByteArray {
        val out = ByteArray(HEADER_BYTES + length + CHECK_BYTES)
        out[0] = type.toByte()
        out[1] = flags.toByte()
        out[2] = stream.toByte()
        out[3] = (stream shr 8).toByte()
        put32(out, 4, length.toLong())
        System.arraycopy(payload, offset, out, HEADER_BYTES, length)
        val crc = CRC32().apply { update(out, 0, HEADER_BYTES + length) }
        put32(out, HEADER_BYTES + length, crc.value)
        return out
    }

    internal fun get32(bytes: ByteArray, at: Int): Long =
        (bytes[at].toLong() and 0xFF) or ((bytes[at + 1].toLong() and 0xFF) shl 8) or
            ((bytes[at + 2].toLong() and 0xFF) shl 16) or ((bytes[at + 3].toLong() and 0xFF) shl 24)

    private fun put32(bytes: ByteArray, at: Int, value: Long) {
        for (i in 0 until 4) bytes[at + i] = (value shr (8 * i)).toByte()
    }
}

/** Turns a stream of bytes, cut anywhere, back into frames. Not safe for use from two threads at once. */
class FrameDecoder {
    /** The largest payload accepted: [FrameCodec.MAX_PAYLOAD], or the lower `maxFrame` once `hello` has set it. */
    @Volatile
    var maxPayload: Int = FrameCodec.MAX_PAYLOAD

    private var buffer = ByteArray(4096)
    private var start = 0
    private var end = 0
    private var failed = false

    fun feed(bytes: ByteArray, offset: Int, length: Int) {
        if (failed) return
        if (start > 0 && (start == end || start >= buffer.size / 2)) {
            System.arraycopy(buffer, start, buffer, 0, end - start)
            end -= start
            start = 0
        }
        if (end + length > buffer.size) buffer = buffer.copyOf(maxOf(buffer.size * 2, end + length))
        System.arraycopy(bytes, offset, buffer, end, length)
        end += length
    }

    /**
     * The next whole frame, or null if more bytes are needed first.
     *
     * @throws FrameException if the bytes are not a frame. Every later call throws too.
     */
    fun next(): Frame? {
        if (failed) throw FrameException("the link has already failed")
        if (end - start < FrameCodec.HEADER_BYTES) return null
        val type = buffer[start].toInt() and 0xFF
        val flags = buffer[start + 1].toInt() and 0xFF
        val stream = (buffer[start + 2].toInt() and 0xFF) or ((buffer[start + 3].toInt() and 0xFF) shl 8)
        val length = FrameCodec.get32(buffer, start + 4)

        // The header is judged before its payload is waited for, so a wrong length is refused at once and
        // never buffered.
        val why = when {
            type != FrameCodec.TYPE_MSG && type != FrameCodec.TYPE_DATA -> "unknown frame type $type"
            flags and FrameCodec.FLAG_COMPRESSED != 0 -> "a compressed payload, which this version does not have"
            flags and (FrameCodec.FLAG_COMPRESSED or FrameCodec.FLAG_LAST).inv() != 0 -> "unknown frame flags $flags"
            type == FrameCodec.TYPE_MSG && flags != 0 -> "a MSG frame with flags"
            type == FrameCodec.TYPE_MSG && stream != 0 -> "a MSG frame with a stream number"
            type == FrameCodec.TYPE_DATA && stream == 0 -> "a DATA frame with no stream number"
            length > maxPayload -> "a frame of $length bytes; the limit is $maxPayload"
            else -> null
        }
        if (why != null) fail(why)

        val body = FrameCodec.HEADER_BYTES + length.toInt()
        if (end - start < body + FrameCodec.CHECK_BYTES) return null
        val crc = CRC32().apply { update(buffer, start, body) }
        if (crc.value != FrameCodec.get32(buffer, start + body)) fail("a frame whose check value is wrong")
        val payload = buffer.copyOfRange(start + FrameCodec.HEADER_BYTES, start + body)
        start += body + FrameCodec.CHECK_BYTES
        return Frame(type, flags, stream, payload)
    }

    private fun fail(why: String): Nothing {
        failed = true
        throw FrameException(why)
    }
}

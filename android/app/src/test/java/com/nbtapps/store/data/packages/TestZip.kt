// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import java.util.zip.Deflater

/**
 * Writes zip archives byte by byte, so a test can break one rule of the package profile at a time.
 * [java.util.zip.ZipOutputStream] refuses to write most of what these tests need.
 */
internal object TestZip {

    class Member(
        val name: String,
        val data: ByteArray,
        val deflate: Boolean = true,
        /** General purpose flags, written to both headers. */
        val flags: Int = 0,
        /** Compression method to declare; defaults to the one actually used. */
        val method: Int? = null,
        /** Name in the header in front of the data, when it should differ from the directory's. */
        val localName: String? = null,
        /** Uncompressed size to declare; defaults to the real one. */
        val declaredSize: Long? = null,
    )

    fun bytes(
        members: List<Member>,
        comment: ByteArray = ByteArray(0),
        /** Entry count to declare in the end record; defaults to the real one. */
        declaredCount: Int? = null,
    ): ByteArray {
        val out = ByteArrayOutputStream()
        val directory = ByteArrayOutputStream()
        for (member in members) {
            val stored = if (member.deflate) deflate(member.data) else member.data
            val method = member.method ?: if (member.deflate) 8 else 0
            val crc = CRC32().apply { update(member.data) }.value
            val size = member.declaredSize ?: member.data.size.toLong()
            val offset = out.size()
            val name = member.name.encodeToByteArray()
            val localName = (member.localName ?: member.name).encodeToByteArray()

            out.u32(0x04034b50)
            out.u16(20) // version needed
            out.u16(member.flags)
            out.u16(method)
            out.u32(0) // time, date
            out.u32(crc)
            out.u32(stored.size.toLong())
            out.u32(size)
            out.u16(localName.size)
            out.u16(0) // extra
            out.write(localName)
            out.write(stored)

            directory.u32(0x02014b50)
            directory.u16(20) // version made by
            directory.u16(20) // version needed
            directory.u16(member.flags)
            directory.u16(method)
            directory.u32(0) // time, date
            directory.u32(crc)
            directory.u32(stored.size.toLong())
            directory.u32(size)
            directory.u16(name.size)
            directory.u16(0) // extra
            directory.u16(0) // comment
            directory.u16(0) // disk
            directory.u16(0) // internal attributes
            directory.u32(0) // external attributes
            directory.u32(offset.toLong())
            directory.write(name)
        }
        val directoryOffset = out.size()
        out.write(directory.toByteArray())

        val count = declaredCount ?: members.size
        out.u32(0x06054b50)
        out.u16(0) // this disk
        out.u16(0) // directory disk
        out.u16(count)
        out.u16(count)
        out.u32(directory.size().toLong())
        out.u32(directoryOffset.toLong())
        out.u16(comment.size)
        out.write(comment)
        return out.toByteArray()
    }

    private fun deflate(data: ByteArray): ByteArray {
        val deflater = Deflater(Deflater.DEFAULT_COMPRESSION, true)
        deflater.setInput(data)
        deflater.finish()
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(4096)
        while (!deflater.finished()) out.write(buffer, 0, deflater.deflate(buffer))
        deflater.end()
        return out.toByteArray()
    }

    private fun ByteArrayOutputStream.u16(value: Int) {
        write(value and 0xFF)
        write((value shr 8) and 0xFF)
    }

    private fun ByteArrayOutputStream.u32(value: Long) {
        u16((value and 0xFFFF).toInt())
        u16(((value shr 16) and 0xFFFF).toInt())
    }
}

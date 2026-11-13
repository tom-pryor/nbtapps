// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.data.crypto.Ed25519
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.crypto.newSha256
import com.nbtapps.store.data.json.Utf8
import com.nbtapps.store.model.PackageManifest
import com.nbtapps.store.model.PackageVersion
import java.io.ByteArrayOutputStream
import java.io.EOFException
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.zip.CRC32
import java.util.zip.DataFormatException
import java.util.zip.Inflater
import kotlin.math.min

/**
 * The file is not a package this app accepts. The message is shown to the user. [code] is the code the
 * head-unit agent reports for the same refusal, so the two readers can be compared case by case.
 */
class PackageFormatException(
    message: String,
    val code: String = BAD_ARCHIVE,
    cause: Throwable? = null,
) : Exception(message, cause) {
    companion object {
        const val BAD_ARCHIVE = "bad-archive"
        const val BAD_MANIFEST = "bad-manifest"
        const val UNSUPPORTED_MANIFEST = "unsupported-manifest"
        const val BAD_SIGNATURE = "bad-signature"
    }
}

/** A `.nbtpkg` that passed [PackageArchive.verify]. */
class VerifiedPackage(
    val manifest: PackageManifest,
    /** The exact bytes of `manifest.json`: what [manifestSha256] and [signature] cover, and what the agent fetches. */
    val manifestBytes: ByteArray,
    val manifestSha256: String,
    /** SHA-256 of the archive file. */
    val sha256: String,
    /** Size of the archive file in bytes. */
    val size: Long,
    /**
     * The 64 bytes of the signature that was checked: the detached one [PackageArchive.verify] was given, or
     * else `manifest.sig`.
     */
    val signature: ByteArray,
    /** The trusted key that verified [signature]. */
    val key: ByteArray,
) {
    /**
     * Checks that this is the archive the index entry [version] of package [packageId] pins.
     *
     * @throws PackageFormatException if the file, its manifest or its identity differ from the entry.
     */
    fun requireMatches(packageId: String, version: PackageVersion) {
        if (size != version.size || sha256 != version.sha256) {
            throw PackageFormatException("The package isn't the file the catalogue lists")
        }
        if (manifestSha256 != version.manifestSha256) {
            throw PackageFormatException("The package's manifest isn't the one the catalogue lists")
        }
        if (manifest.id != packageId || manifest.version != version.version) {
            throw PackageFormatException(
                "The package says it is ${manifest.id} ${manifest.version}, not $packageId ${version.version}",
            )
        }
    }
}

/**
 * Reads `.nbtpkg` files. A package is a zip archive used as a plain container under a strict profile:
 *
 *  - `manifest.json` is the first entry and `manifest.sig` the second: the source's Ed25519 signature, 64
 *    bytes, over the exact bytes of the manifest. A catalogue may instead give a detached signature over those
 *    bytes, for a package whose author signed it with a key of their own: that one is then checked, and
 *    `manifest.sig` is not read (it may be missing), though where it is present it is still the second entry
 *  - every other file is listed in the manifest's `contents` with its size and the SHA-256 of its
 *    uncompressed bytes, and nothing unlisted is present
 *  - entries are stored or deflated; no zip64, no encryption, no data descriptor, no duplicate name, no
 *    archive comment
 *  - each header in front of a file matches the central directory, and entries sit one after another in
 *    directory order from the first byte of the file, with nothing between them or after the end record
 *  - names are relative, use `/`, have no empty, `.` or `..` segment and are at most 255 bytes long; folder
 *    entries are ignored
 *
 * The archive is parsed here instead of with [java.util.zip.ZipFile] so that anything outside the profile
 * is refused, not tolerated. The head-unit agent enforces the same rules with its own reader
 * (`agent/package.cpp`), in the same order, and an archive that two readers understand differently must
 * not get through either. Both are tested against one set of good and broken packages.
 */
object PackageArchive {
    const val MANIFEST_NAME = "manifest.json"
    const val SIGNATURE_NAME = "manifest.sig"

    private const val END_RECORD_SIGNATURE = 0x06054b50L
    private const val END_RECORD_BYTES = 22
    private const val DIRECTORY_SIGNATURE = 0x02014b50L
    private const val DIRECTORY_HEADER_BYTES = 46
    private const val LOCAL_SIGNATURE = 0x04034b50L
    private const val LOCAL_HEADER_BYTES = 30

    private const val METHOD_STORED = 0
    private const val METHOD_DEFLATED = 8

    /** Bit 0: encrypted. Bit 6: strong encryption. Bit 13: directory values masked. */
    private const val ENCRYPTION_FLAGS = 0x2041

    /** Bit 3: the sizes follow the data instead of standing in the header in front of it. */
    private const val DESCRIPTOR_FLAG = 0x0008

    /** The values zip64 puts in the 16- and 32-bit fields it replaces. */
    private const val ZIP64_U16 = 0xFFFF
    private const val ZIP64_U32 = 0xFFFFFFFFL

    private const val BUFFER_BYTES = 64 * 1024

    /**
     * Checks [file] against the container profile, its signature against [trustedKeys] and its files against
     * its own manifest, reading every byte of it.
     *
     * A package that passes is intact, self-consistent and signed by one of the keys. Whether it is the one
     * a catalogue lists is a separate question: see [VerifiedPackage.requireMatches].
     *
     * With [detachedSignature], that is the signature checked over the manifest, in place of `manifest.sig`.
     *
     * @throws PackageFormatException if the file breaks the profile, is damaged, is not signed by a trusted
     *   key or disagrees with its manifest.
     * @throws IOException if the file can't be read.
     */
    @Throws(IOException::class)
    fun verify(
        file: File,
        trustedKeys: List<ByteArray>,
        detachedSignature: ByteArray? = null,
    ): VerifiedPackage = RandomAccessFile(file, "r").use { archive ->
        // 1. The container. With no archive comment the end record is exactly the last 22 bytes.
        val length = archive.length()
        if (length < END_RECORD_BYTES) fail("The file is too short to be a package")
        val end = ByteArray(END_RECORD_BYTES)
        archive.seek(length - END_RECORD_BYTES)
        archive.readFully(end)
        if (u32(end, 0) != END_RECORD_SIGNATURE) fail("The file isn't a package, or it is cut short")
        val disk = u16(end, 4)
        val directoryDisk = u16(end, 6)
        val entriesOnDisk = u16(end, 8)
        val entryCount = u16(end, 10)
        val directoryBytes = u32(end, 12)
        val directoryOffset = u32(end, 16)
        val commentBytes = u16(end, 20)
        if (entryCount == ZIP64_U16 || directoryBytes == ZIP64_U32 || directoryOffset == ZIP64_U32) {
            fail("The package uses zip64, which isn't allowed")
        }
        if (disk != 0 || directoryDisk != 0 || entriesOnDisk != entryCount || commentBytes != 0) {
            fail("The package's archive layout isn't allowed")
        }
        if (directoryOffset + directoryBytes != length - END_RECORD_BYTES) fail("The package's file list is damaged")

        val entries = readDirectory(archive, directoryOffset, directoryBytes, entryCount)
        val read = readEntries(archive, entries, directoryOffset, length)

        // 2. The manifest is the first entry.
        if (entries.isEmpty() || entries[0].name != MANIFEST_NAME) fail("The package doesn't start with its manifest")
        if (entries[0].size > ManifestParser.MAX_MANIFEST_BYTES) {
            fail("The package's manifest is too large", PackageFormatException.BAD_MANIFEST)
        }
        val manifestBytes = requireNotNull(read.manifest)

        // 3. The signature is the second, and is checked before the manifest is parsed. A detached one is checked
        //    in its place, and manifest.sig is then not read; where it is present, it is still the second entry.
        val hasSignature = entries.size >= 2 && entries[1].name == SIGNATURE_NAME
        if (!hasSignature && entries.any { it.name == SIGNATURE_NAME }) fail("The package's signature isn't where it belongs")
        val signature = when {
            detachedSignature != null -> detachedSignature.takeIf { it.size == Ed25519.SIGNATURE_BYTES }
                ?: fail("The catalogue's signature for the package isn't 64 bytes long", PackageFormatException.BAD_SIGNATURE)
            !hasSignature -> fail("The package isn't signed", PackageFormatException.BAD_SIGNATURE)
            else -> read.signature ?: fail("The package's signature isn't 64 bytes long", PackageFormatException.BAD_SIGNATURE)
        }
        val key = trustedKeys.firstOrNull { Ed25519.verify(it, manifestBytes, signature) }
            ?: fail("The package isn't signed by a key this app trusts", PackageFormatException.BAD_SIGNATURE)

        // 4. The manifest.
        val manifest = ManifestParser.parse(manifestBytes)

        // 5. The files are exactly the ones the manifest lists.
        val files = entries.drop(if (hasSignature) 2 else 1).filter { !it.isFolder }.associateBy { it.name }
        for (content in manifest.contents) {
            val entry = files[content.path] ?: fail("The package's manifest lists ${content.path}, which isn't in the package")
            if (entry.size != content.size || Hex.encode(entry.sha256) != content.sha256) {
                fail("${content.path} isn't the file the package's manifest describes")
            }
        }
        if (files.size != manifest.contents.size) {
            val listed = manifest.contents.mapTo(HashSet()) { it.path }
            val extra = files.keys.first { it !in listed }
            fail("The package holds $extra, which its manifest doesn't list")
        }

        VerifiedPackage(
            manifest = manifest,
            manifestBytes = manifestBytes,
            manifestSha256 = Hex.encode(entries[0].sha256),
            sha256 = read.archiveSha256,
            size = length,
            signature = signature,
            key = key,
        )
    }

    private class Entry(
        val name: String,
        val nameBytes: ByteArray,
        val flags: Int,
        val method: Int,
        val crc: Long,
        val compressedSize: Long,
        val size: Long,
        val localHeaderOffset: Long,
    ) {
        val isFolder: Boolean get() = name.endsWith("/")
        var sha256 = ByteArray(0)
    }

    private class EntriesRead(val manifest: ByteArray?, val signature: ByteArray?, val archiveSha256: String)

    /**
     * Reads a stretch of the file from front to back through one buffer. With a hash given, every byte that
     * passes goes into it: that is how the archive's own SHA-256 comes out of the pass that checks the entries.
     */
    private class Cursor(
        private val file: RandomAccessFile,
        begin: Long,
        private val end: Long,
        private val hash: MessageDigest?,
    ) {
        var at = begin
            private set
        val buffer = ByteArray(BUFFER_BYTES)

        /** Where in [buffer] the bytes of the last [next] start. */
        var start = 0
            private set
        private var have = 0
        private var used = 0

        /** The next bytes, at most [max] of them: at least one unless the stretch has ended. */
        fun next(max: Long): Int {
            if (used == have) {
                have = min(end - at, buffer.size.toLong()).toInt()
                used = 0
                if (have > 0) {
                    file.seek(at)
                    file.readFully(buffer, 0, have)
                }
            }
            val count = min((have - used).toLong(), max).toInt()
            start = used
            if (count > 0) hash?.update(buffer, start, count)
            used += count
            at += count
            return count
        }

        /** Exactly [out].size bytes. */
        fun take(out: ByteArray) {
            var filled = 0
            while (filled < out.size) {
                val count = next((out.size - filled).toLong())
                if (count == 0) throw EOFException()
                System.arraycopy(buffer, start, out, filled, count)
                filled += count
            }
        }

        fun skip(length: Long) {
            var left = length
            while (left > 0) {
                val count = next(left)
                if (count == 0) throw EOFException()
                left -= count
            }
        }
    }

    /** Reads the central directory, refusing everything the profile leaves out. */
    private fun readDirectory(archive: RandomAccessFile, offset: Long, bytes: Long, count: Int): List<Entry> {
        val damaged = "The package's file list is damaged"
        val end = offset + bytes
        val cursor = Cursor(archive, offset, end, null)
        val names = HashSet<String>()
        val entries = ArrayList<Entry>(count)
        val header = ByteArray(DIRECTORY_HEADER_BYTES)
        repeat(count) {
            if (cursor.at + DIRECTORY_HEADER_BYTES > end) fail(damaged)
            cursor.take(header)
            val flags = u16(header, 8)
            val method = u16(header, 10)
            val crc = u32(header, 16)
            val compressedSize = u32(header, 20)
            val size = u32(header, 24)
            val nameBytes = u16(header, 28)
            val extraBytes = u16(header, 30)
            val entryCommentBytes = u16(header, 32)
            val startDisk = u16(header, 34)
            val localHeaderOffset = u32(header, 42)
            if (u32(header, 0) != DIRECTORY_SIGNATURE || cursor.at + nameBytes + extraBytes + entryCommentBytes > end) fail(damaged)
            if (nameBytes > ManifestParser.MAX_PATH_BYTES) fail("The package holds a path longer than 255 bytes")
            val rawName = ByteArray(nameBytes).also { cursor.take(it) }
            cursor.skip((extraBytes + entryCommentBytes).toLong())

            if (!Utf8.isValid(rawName)) fail("The package holds a file name that isn't valid text")
            val name = String(rawName, Charsets.UTF_8)
            val folder = name.endsWith("/")
            if (!ManifestParser.isRelativePath(name, folder)) fail("The package holds a path that isn't allowed: $name")
            if (!names.add(name)) fail("The package holds $name twice")
            if (flags and ENCRYPTION_FLAGS != 0) fail("$name is encrypted, which isn't allowed")
            if (flags and DESCRIPTOR_FLAG != 0) fail("$name has its sizes after its data, which isn't allowed")
            if (method != METHOD_STORED && method != METHOD_DEFLATED) fail("$name uses a compression that isn't allowed")
            if (compressedSize == ZIP64_U32 || size == ZIP64_U32 || localHeaderOffset == ZIP64_U32) {
                fail("The package uses zip64, which isn't allowed")
            }
            if (startDisk != 0) fail("The package's archive layout isn't allowed")
            if (folder && (method != METHOD_STORED || compressedSize != 0L || size != 0L)) fail("The folder entry $name holds data")
            entries += Entry(name, rawName, flags, method, crc, compressedSize, size, localHeaderOffset)
        }
        if (cursor.at != end) fail(damaged)
        return entries
    }

    /**
     * Walks the entries from byte 0 to the directory, checking each header against the directory and each
     * file against its size and CRC, and takes every file's SHA-256 on the way. Entries sit one after another
     * in directory order, so a reader that walks the headers of a stream and a reader that starts from the
     * directory see the same files.
     */
    private fun readEntries(archive: RandomAccessFile, entries: List<Entry>, directoryOffset: Long, length: Long): EntriesRead {
        val archiveHash = newSha256()
        val cursor = Cursor(archive, 0, directoryOffset, archiveHash)
        var manifest: ByteArray? = null
        var signature: ByteArray? = null
        var expected = 0L
        val header = ByteArray(LOCAL_HEADER_BYTES)
        entries.forEachIndexed { index, entry ->
            val damaged = "${entry.name} is damaged"
            if (entry.localHeaderOffset != expected) fail("${entry.name} isn't where the file before it ends")
            if (expected + LOCAL_HEADER_BYTES > directoryOffset) fail(damaged)
            cursor.take(header)
            val nameBytes = u16(header, 26)
            val extraBytes = u16(header, 28)
            val dataOffset = expected + LOCAL_HEADER_BYTES + nameBytes + extraBytes
            if (u32(header, 0) != LOCAL_SIGNATURE || dataOffset + entry.compressedSize > directoryOffset) fail(damaged)
            val localName = ByteArray(nameBytes).also { cursor.take(it) }
            // Readers that trust this header and readers that trust the directory must see the same file.
            if (u16(header, 6) != entry.flags || u16(header, 8) != entry.method || u32(header, 14) != entry.crc ||
                u32(header, 18) != entry.compressedSize || u32(header, 22) != entry.size ||
                !localName.contentEquals(entry.nameBytes)
            ) {
                fail("${entry.name} is described in two different ways")
            }
            cursor.skip(extraBytes.toLong())

            // The manifest and its signature are the only entries kept in memory, and only at sizes that can be right.
            val keep = when {
                index == 0 && entry.name == MANIFEST_NAME && entry.size <= ManifestParser.MAX_MANIFEST_BYTES ->
                    ByteArrayOutputStream(entry.size.toInt())
                index == 1 && entry.name == SIGNATURE_NAME && entry.size == Ed25519.SIGNATURE_BYTES.toLong() ->
                    ByteArrayOutputStream(Ed25519.SIGNATURE_BYTES)
                else -> null
            }
            entry.sha256 = readData(entry, cursor, keep)
            if (index == 0) manifest = keep?.toByteArray()
            if (index == 1) signature = keep?.toByteArray()
            expected = dataOffset + entry.compressedSize
        }
        if (expected != directoryOffset) fail("The package holds bytes that belong to no file")

        // The rest of the archive's own hash: the directory and the end record.
        Cursor(archive, directoryOffset, length, archiveHash).skip(length - directoryOffset)
        return EntriesRead(manifest, signature, Hex.encode(archiveHash.digest()))
    }

    /** Reads the data of [entry] from [cursor] and returns the SHA-256 of its uncompressed bytes. */
    private fun readData(entry: Entry, cursor: Cursor, keep: ByteArrayOutputStream?): ByteArray {
        val damaged = "${entry.name} is damaged"
        val hash = newSha256()
        val crc = CRC32()
        var produced = 0L
        var remaining = entry.compressedSize
        fun emit(bytes: ByteArray, offset: Int, count: Int) {
            produced += count
            if (produced > entry.size) fail(damaged)
            hash.update(bytes, offset, count)
            crc.update(bytes, offset, count)
            keep?.write(bytes, offset, count)
        }

        if (entry.method == METHOD_STORED) {
            if (entry.compressedSize != entry.size) fail(damaged)
            while (remaining > 0) {
                val count = cursor.next(remaining)
                if (count == 0) throw EOFException()
                remaining -= count
                emit(cursor.buffer, cursor.start, count)
            }
        } else {
            val inflater = Inflater(true)
            try {
                val output = ByteArray(BUFFER_BYTES)
                var done = false
                while (!done) {
                    // The stream must end exactly where the entry does: not before its last byte and not after it.
                    if (remaining == 0L) fail(damaged)
                    val count = cursor.next(remaining)
                    if (count == 0) throw EOFException()
                    remaining -= count
                    inflater.setInput(cursor.buffer, cursor.start, count)
                    while (true) {
                        val got = try {
                            inflater.inflate(output)
                        } catch (e: DataFormatException) {
                            fail(damaged, cause = e)
                        }
                        if (got > 0) emit(output, 0, got)
                        if (inflater.finished()) {
                            if (inflater.remaining != 0 || remaining != 0L) fail(damaged)
                            done = true
                            break
                        }
                        if (inflater.needsDictionary()) fail(damaged)
                        if (inflater.needsInput()) break
                        if (got == 0) fail(damaged)
                    }
                }
            } finally {
                inflater.end()
            }
        }
        if (produced != entry.size || crc.value != entry.crc) fail(damaged)
        return hash.digest()
    }

    private fun u16(bytes: ByteArray, at: Int): Int = (bytes[at].toInt() and 0xFF) or ((bytes[at + 1].toInt() and 0xFF) shl 8)

    private fun u32(bytes: ByteArray, at: Int): Long = u16(bytes, at).toLong() or (u16(bytes, at + 2).toLong() shl 16)

    private fun fail(
        message: String,
        code: String = PackageFormatException.BAD_ARCHIVE,
        cause: Throwable? = null,
    ): Nothing = throw PackageFormatException(message, code, cause)
}

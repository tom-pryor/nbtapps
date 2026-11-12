// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.TestKeys
import com.nbtapps.store.data.crypto.sha256Hex
import com.nbtapps.store.data.packages.TestZip.Member
import com.nbtapps.store.model.PackageVersion
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class PackageArchiveTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val keys = TestKeys()

    private val files = mapOf(
        "payload/bin/hello" to ByteArray(200_000) { (it * 7).toByte() },
        "payload/etc/hello.conf" to "greeting = hello\n".encodeToByteArray(),
        "scripts/post-install.sh" to "#!/bin/ksh\nexit 0\n".encodeToByteArray(),
    )

    private fun contents(files: Map<String, ByteArray>): String =
        files.entries.joinToString(",") { (path, data) ->
            """{ "path": "$path", "size": ${data.size}, "sha256": "${sha256Hex(data)}" }"""
        }

    private fun manifest(
        contents: String = contents(files),
        id: String = "org.example.app",
        version: String = "1.2.0",
        manifestVersion: Int = 1,
        install: String = """{ "files": [{ "src": "payload/bin/hello", "dest": "${'$'}{app}/bin/hello", "mode": "0755" }] }""",
    ): ByteArray =
        """
        { "manifestVersion": $manifestVersion, "id": "$id", "name": "App", "version": "$version",
          "requires": { "agent": ">=1.0.0" }, "depends": [{ "id": "org.example.lib", "version": ">=2.0.0" }],
          "contents": [$contents],
          "install": $install }
        """.trimIndent().encodeToByteArray()

    /** The manifest, its signature, then [files] in order. */
    private fun members(
        manifest: ByteArray = manifest(),
        files: Map<String, ByteArray> = this.files,
        signature: ByteArray = keys.sign(manifest),
    ): List<Member> =
        listOf(Member(PackageArchive.MANIFEST_NAME, manifest), Member(PackageArchive.SIGNATURE_NAME, signature, deflate = false)) +
            files.map { (path, data) -> Member(path, data) }

    private fun write(bytes: ByteArray): File = folder.newFile().apply { writeBytes(bytes) }

    private fun verify(bytes: ByteArray): VerifiedPackage = PackageArchive.verify(write(bytes), listOf(keys.publicKey))

    private fun verify(members: List<Member>): VerifiedPackage = verify(TestZip.bytes(members))

    private fun assertRefused(code: String, members: List<Member>) = assertRefused(code, TestZip.bytes(members))

    private fun assertRefused(code: String, bytes: ByteArray) {
        val refusal = assertThrows(PackageFormatException::class.java) { verify(bytes) }
        assertEquals(refusal.message, code, refusal.code)
    }

    private val archive = PackageFormatException.BAD_ARCHIVE
    private val badManifest = PackageFormatException.BAD_MANIFEST
    private val badSignature = PackageFormatException.BAD_SIGNATURE

    @Test
    fun `a well-formed package verifies and reports its manifest, hashes and signature`() {
        val manifestBytes = manifest()
        val bytes = TestZip.bytes(members(manifestBytes))

        val verified = verify(bytes)

        assertEquals("org.example.app", verified.manifest.id)
        assertEquals("1.2.0", verified.manifest.version)
        assertEquals(">=1.0.0", verified.manifest.requires.agent)
        assertEquals("org.example.lib", verified.manifest.depends.single().id)
        assertEquals(files.keys.toList(), verified.manifest.contents.map { it.path })
        assertArrayEquals(manifestBytes, verified.manifestBytes)
        assertEquals(sha256Hex(manifestBytes), verified.manifestSha256)
        assertEquals(sha256Hex(bytes), verified.sha256)
        assertEquals(bytes.size.toLong(), verified.size)
        assertArrayEquals(keys.sign(manifestBytes), verified.signature)
        assertArrayEquals(keys.publicKey, verified.key)
    }

    @Test
    fun `stored entries and folder entries are accepted`() {
        val manifest = manifest()
        val members = listOf(
            Member(PackageArchive.MANIFEST_NAME, manifest, deflate = false),
            Member(PackageArchive.SIGNATURE_NAME, keys.sign(manifest), deflate = false),
            Member("payload/", ByteArray(0), deflate = false),
        ) + files.map { (path, data) -> Member(path, data, deflate = false) }

        assertEquals(3, verify(members).manifest.contents.size)
    }

    @Test
    fun `the first trusted key that verifies is reported, and a package signed by none is refused`() {
        val other = TestKeys()
        val bytes = TestZip.bytes(members())

        assertArrayEquals(keys.publicKey, PackageArchive.verify(write(bytes), listOf(other.publicKey, keys.publicKey)).key)

        val refusal = assertThrows(PackageFormatException::class.java) {
            PackageArchive.verify(write(bytes), listOf(other.publicKey))
        }
        assertEquals(badSignature, refusal.code)
        assertEquals(badSignature, assertThrows(PackageFormatException::class.java) { PackageArchive.verify(write(bytes), emptyList()) }.code)
    }

    @Test
    fun `an unsigned package, a signature in the wrong place and a signature of the wrong size are refused`() {
        val manifest = manifest()
        val payload = files.map { (path, data) -> Member(path, data) }
        val signature = Member(PackageArchive.SIGNATURE_NAME, keys.sign(manifest), deflate = false)

        assertRefused(badSignature, listOf(Member(PackageArchive.MANIFEST_NAME, manifest)) + payload)
        assertRefused(archive, listOf(Member(PackageArchive.MANIFEST_NAME, manifest)) + payload + signature)
        assertRefused(badSignature, members(manifest, signature = keys.sign(manifest).copyOf(63)))
        assertRefused(badSignature, members(manifest, signature = keys.sign(manifest) + 0))
    }

    @Test
    fun `a manifest changed after signing is refused before it is read`() {
        val signed = manifest()
        val changed = manifest(version = "9.9.9")
        assertRefused(badSignature, members(changed, signature = keys.sign(signed)))
        // Not even a manifest that would fail to parse gets a different answer: the signature comes first.
        assertRefused(badSignature, members("{ not json".encodeToByteArray(), signature = keys.sign(signed)))
    }

    @Test
    fun `an archive written by the platform zip writer is refused`() {
        // ZipOutputStream leaves the sizes out of the header in front of each file and writes them after the
        // data. A reader of a stream cannot tell where such a file ends, so the profile forbids it.
        val manifest = manifest()
        val out = ByteArrayOutputStream()
        ZipOutputStream(out).use { zip ->
            zip.putNextEntry(ZipEntry(PackageArchive.MANIFEST_NAME))
            zip.write(manifest)
            zip.putNextEntry(ZipEntry(PackageArchive.SIGNATURE_NAME))
            zip.write(keys.sign(manifest))
            files.forEach { (path, data) ->
                zip.putNextEntry(ZipEntry(path))
                zip.write(data)
            }
        }
        assertRefused(archive, out.toByteArray())
    }

    @Test
    fun `the manifest must be the first entry`() {
        assertRefused(archive, members().drop(1) + members().first())
        assertRefused(archive, members().drop(1))
    }

    @Test
    fun `a file the manifest doesn't list is refused`() {
        assertRefused(archive, members() + Member("payload/extra", byteArrayOf(1)))
    }

    @Test
    fun `a listed file that is missing is refused`() {
        assertRefused(archive, members().dropLast(1))
    }

    @Test
    fun `a file that differs from its listed hash or size is refused`() {
        val signed = manifest()
        val changed = files + ("payload/etc/hello.conf" to "greeting = hullo\n".encodeToByteArray())
        assertRefused(archive, members(signed, changed))

        val longer = files + ("payload/etc/hello.conf" to "greeting = hello\n\n".encodeToByteArray())
        assertRefused(archive, members(signed, longer))
    }

    @Test
    fun `a manifest that lists a path twice, a bad hash, itself or its signature is refused`() {
        assertRefused(badManifest, members(manifest(contents(files) + "," + contents(files.entries.first().let { mapOf(it.toPair()) }))))
        assertRefused(badManifest, members(manifest("""{ "path": "payload/bin/hello", "size": 200000, "sha256": "abc" }""")))
        assertRefused(badManifest, members(manifest(contents(files + (PackageArchive.MANIFEST_NAME to manifest())))))
        assertRefused(badManifest, members(manifest(contents(files + (PackageArchive.SIGNATURE_NAME to ByteArray(64))))))
    }

    @Test
    fun `a duplicate name is refused`() {
        val (path, data) = files.entries.first()
        assertRefused(archive, members() + Member(path, data))
    }

    @Test
    fun `paths that climb out or are absolute are refused`() {
        for (path in listOf("../evil", "payload/../../evil", "/etc/passwd", "payload//x", "payload\\\\x", "./x")) {
            val escaping = mapOf(path to byteArrayOf(1))
            assertRefused(archive, members(manifest(contents(escaping), install = "{}"), escaping))
        }
    }

    @Test
    fun `a name longer than 255 bytes is refused`() {
        val long = mapOf("payload/" + "n".repeat(248) to byteArrayOf(1))
        assertRefused(archive, members(manifest(contents(long), install = "{}"), long))
        val longest = mapOf("payload/" + "n".repeat(247) to byteArrayOf(1))
        assertEquals(1, verify(members(manifest(contents(longest), install = "{}"), longest)).manifest.contents.size)
    }

    @Test
    fun `an encrypted entry is refused`() {
        val (path, data) = files.entries.first()
        assertRefused(archive, members().filterNot { it.name == path } + Member(path, data, flags = 0x1))
    }

    @Test
    fun `a compression other than deflate is refused`() {
        val (path, data) = files.entries.first()
        assertRefused(archive, members().filterNot { it.name == path } + Member(path, data, deflate = false, method = 12))
    }

    @Test
    fun `an entry whose two headers disagree is refused`() {
        val (path, data) = files.entries.first()
        assertRefused(archive, members().filterNot { it.name == path } + Member(path, data, localName = "payload/bin/other"))
    }

    @Test
    fun `an entry that inflates to a different size than declared is refused`() {
        val (path, data) = files.entries.first()
        assertRefused(archive, members().filterNot { it.name == path } + Member(path, data, declaredSize = data.size - 1L))
        assertRefused(archive, members().filterNot { it.name == path } + Member(path, data, declaredSize = data.size + 1L))
    }

    @Test
    fun `an archive comment is refused`() {
        assertRefused(archive, TestZip.bytes(members(), comment = "hello".encodeToByteArray()))
    }

    @Test
    fun `zip64 markers are refused`() {
        assertRefused(archive, TestZip.bytes(members(), declaredCount = 0xFFFF))
    }

    @Test
    fun `a damaged or truncated file is refused`() {
        val bytes = TestZip.bytes(members())

        assertRefused(archive, bytes.copyOf(bytes.size - 10))
        assertRefused(archive, ByteArray(0))
        assertRefused(archive, "not a package at all, just text".encodeToByteArray())

        // One byte flipped inside the first payload file's compressed data.
        val flipped = bytes.copyOf().also { it[it.size / 2] = (it[it.size / 2] + 1).toByte() }
        assertRefused(archive, flipped)
    }

    @Test
    fun `a malformed manifest or an unknown manifest version is refused`() {
        assertRefused(badManifest, members("{ not json".encodeToByteArray()))
        assertRefused(badManifest, members("""{ "manifestVersion": 1, "id": "a.b", "name": "A", "version": "1.0.0" }""".encodeToByteArray(), emptyMap()))
        assertRefused(PackageFormatException.UNSUPPORTED_MANIFEST, members(manifest(manifestVersion = 2)))
        assertRefused(badManifest, members(manifest(id = "Not An Id")))
        assertRefused(badManifest, members(manifest(id = "oneword")))
        assertRefused(badManifest, members(manifest(version = "latest")))
        // The same key twice: a general JSON reader would quietly keep one of them.
        assertRefused(badManifest, members(manifest().decodeToString().replaceFirst("{", """{ "id": "org.example.other",""").encodeToByteArray()))
    }

    @Test
    fun `an install key the app doesn't know is refused, and an unknown key elsewhere is ignored`() {
        assertRefused(
            PackageFormatException.UNSUPPORTED_MANIFEST,
            members(manifest(install = """{ "files": [], "services": [] }""")),
        )
        val extra = manifest().decodeToString().replaceFirst("{", """{ "homepage": "https://example.invalid",""").encodeToByteArray()
        assertEquals("org.example.app", verify(members(extra)).manifest.id)
    }

    @Test
    fun `the risk flags come from the install plan`() {
        fun flags(install: String) = verify(members(manifest(install = install))).manifest.flags

        assertEquals(emptyList<String>(), flags("""{ "files": [{ "src": "payload/bin/hello", "dest": "${'$'}{app}/hello", "mode": "0755" }] }"""))
        assertEquals(
            listOf("runs-scripts"),
            flags("""{ "hooks": { "postInstall": { "script": "scripts/post-install.sh", "timeout": 30 } } }"""),
        )
        assertEquals(
            listOf("modifies-system"),
            flags("""{ "files": [{ "src": "payload/etc/hello.conf", "dest": "/etc/hello.conf", "mode": "0644" }] }"""),
        )
        assertEquals(
            listOf("modifies-system"),
            flags(
                """{ "files": [{ "src": "payload/etc/hello.conf", "dest": "${'$'}{data}/hello.conf", "mode": "0644",
                     "replaces": { "sha256": ["${"0".repeat(64)}"] } }] }""",
            ),
        )
        assertEquals(listOf("modifies-system"), flags("""{ "links": [{ "path": "/usr/bin/hello", "target": "x" }] }"""))
        assertEquals(listOf("needs-reboot"), flags("""{ "reboot": "hmi-restart" }"""))
        assertEquals(
            listOf("runs-scripts", "modifies-system", "needs-reboot"),
            flags(
                """{ "files": [{ "src": "payload/", "dest": "/opt/app/", "mode": "0644" }],
                     "hooks": { "preRemove": { "script": "scripts/post-install.sh", "timeout": 1 } }, "reboot": "required" }""",
            ),
        )
    }

    @Test
    fun `a verified package is matched against its index entry`() {
        val verified = verify(members())
        val entry = PackageVersion(
            version = "1.2.0",
            url = "https://example.invalid/app.nbtpkg",
            size = verified.size,
            sha256 = verified.sha256,
            manifestSha256 = verified.manifestSha256,
        )

        verified.requireMatches("org.example.app", entry)

        assertThrows(PackageFormatException::class.java) { verified.requireMatches("org.example.other", entry) }
        assertThrows(PackageFormatException::class.java) { verified.requireMatches("org.example.app", entry.copy(version = "1.3.0")) }
        assertThrows(PackageFormatException::class.java) { verified.requireMatches("org.example.app", entry.copy(size = entry.size + 1)) }
        assertThrows(PackageFormatException::class.java) {
            verified.requireMatches("org.example.app", entry.copy(sha256 = "0".repeat(64)))
        }
        assertThrows(PackageFormatException::class.java) {
            verified.requireMatches("org.example.app", entry.copy(manifestSha256 = "0".repeat(64)))
        }
    }
}

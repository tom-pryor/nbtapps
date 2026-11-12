// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.crypto.sha256Hex
import com.nbtapps.store.data.link.Blob
import com.nbtapps.store.model.PackageVersion
import java.io.File
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class PackageStoreTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val dir by lazy { folder.newFolder("packages") }
    private val keys = listOf(AgentVectors.packageKey)
    private val runtime = AgentVectors.file("packages/good/org.nbtapp.runtime-2.1.0.nbtpkg")
    private val hello = AgentVectors.file("packages/good/org.nbtapp.hello-1.2.0.nbtpkg")

    private fun leftovers(): List<String> = dir.list().orEmpty().sorted()

    @Test
    fun `a verified archive is stored under its hash, with its manifest and what the app needs to know of it`() = runTest {
        val store = PackageStore(dir)
        store.load()
        var progress = 0L

        val stored = runtime.inputStream().use { store.add(it, keys) { bytes -> progress = bytes } }

        assertEquals("org.nbtapp.runtime", stored.id)
        assertEquals("NBT Runtime", stored.name)
        assertEquals("2.1.0", stored.version)
        assertEquals(sha256Hex(runtime.readBytes()), stored.sha256)
        assertEquals(runtime.length(), stored.size)
        assertEquals(runtime.length(), progress)
        assertEquals(Hex.encode(AgentVectors.packageKey), stored.key)
        assertEquals(">=1.0.0", stored.requiresAgent)
        assertEquals(listOf(stored), store.packages.value)
        assertEquals(stored, store.find("org.nbtapp.runtime", "2.1.0"))
        assertNull(store.find("org.nbtapp.runtime", "2.0.0"))
        assertArrayEquals(runtime.readBytes(), store.archive(stored).readBytes())
        assertEquals(listOf("${stored.sha256}.json", "${stored.sha256}.manifest", "${stored.sha256}.nbtpkg"), leftovers())

        // What the agent is offered, and what it may then fetch.
        val offer = stored.offer()
        assertEquals(stored.sha256, offer.sha256)
        assertEquals(128, offer.signature.length)
        val blobs = store.blobs(stored)
        assertEquals(setOf(stored.sha256, stored.manifestSha256), blobs.keys)
        assertEquals(stored.manifestSha256, sha256Hex((blobs.getValue(stored.manifestSha256) as Blob.OfBytes).bytes))
        assertEquals(stored.size, blobs.getValue(stored.sha256).size)
    }

    @Test
    fun `what is stored is found again by a store that starts later`() = runTest {
        val first = PackageStore(dir).also { it.load() }
        val a = hello.inputStream().use { first.add(it, keys) }
        val b = runtime.inputStream().use { first.add(it, keys) }
        File(dir, "download-123.part").writeText("cut off")

        val second = PackageStore(dir)
        assertTrue(second.packages.value.isEmpty())
        second.load()
        second.awaitLoaded()

        assertEquals(listOf(a, b), second.packages.value)
        assertTrue("a download that was cut off is cleared away", leftovers().none { it.endsWith(".part") })
    }

    @Test
    fun `an archive missing its file or its record is not listed`() = runTest {
        val first = PackageStore(dir).also { it.load() }
        val a = hello.inputStream().use { first.add(it, keys) }
        val b = runtime.inputStream().use { first.add(it, keys) }
        first.archive(a).delete()
        File(dir, "${b.sha256}.json").writeText("{ not json")

        val second = PackageStore(dir).also { it.load() }
        assertTrue(second.packages.value.isEmpty())
    }

    @Test
    fun `an archive that is refused leaves nothing behind`() = runTest {
        val store = PackageStore(dir).also { it.load() }
        for (bad in listOf("signature-flipped", "file-changed", "data-descriptor", "json-duplicate-key")) {
            val file = AgentVectors.file("packages/bad/$bad.nbtpkg")
            assertThrows(bad, PackageFormatException::class.java) { kotlinx.coroutines.runBlocking { file.inputStream().use { store.add(it, keys) } } }
        }
        // Signed by a key this source does not have.
        assertThrows(PackageFormatException::class.java) {
            kotlinx.coroutines.runBlocking { runtime.inputStream().use { store.add(it, listOf(AgentVectors.otherKey)) } }
        }
        assertTrue(store.packages.value.isEmpty())
        assertEquals(emptyList<String>(), leftovers())
    }

    @Test
    fun `an archive must be the one the catalogue entry pins`() = runTest {
        val store = PackageStore(dir).also { it.load() }
        val bytes = runtime.readBytes()
        val verified = PackageArchive.verify(runtime, keys)
        val entry = PackageVersion("2.1.0", url = "https://example.invalid/r.nbtpkg", size = bytes.size.toLong(), sha256 = verified.sha256, manifestSha256 = verified.manifestSha256)
        fun add(id: String, version: PackageVersion) = kotlinx.coroutines.runBlocking { runtime.inputStream().use { store.add(it, keys, id to version) } }

        assertThrows(PackageFormatException::class.java) { add("org.nbtapp.other", entry) }
        assertThrows(PackageFormatException::class.java) { add("org.nbtapp.runtime", entry.copy(sha256 = "0".repeat(64))) }
        assertThrows(PackageFormatException::class.java) { add("org.nbtapp.runtime", entry.copy(manifestSha256 = "0".repeat(64))) }
        // More bytes than the entry says is refused while copying, before anything is verified.
        assertThrows(PackageFormatException::class.java) { add("org.nbtapp.runtime", entry.copy(size = entry.size - 1)) }
        assertEquals(emptyList<String>(), leftovers())

        assertNotNull(add("org.nbtapp.runtime", entry))
    }

    @Test
    fun `deleting removes the archive from the phone`() = runTest {
        val store = PackageStore(dir).also { it.load() }
        val stored = runtime.inputStream().use { store.add(it, keys) }

        store.delete(stored)

        assertTrue(store.packages.value.isEmpty())
        assertEquals(emptyList<String>(), leftovers())
    }
}

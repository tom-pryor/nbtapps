// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.TestKeys
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DefaultCatalogueRepositoryTest {

    private val keys = TestKeys()

    /** Serves [files], with a signature of the index made as it is asked for, unless one is set. */
    private inner class FakeSource : CatalogueSource {
        val files = mutableMapOf<String, String>()
        var failure: IOException? = null
        var signature: ByteArray? = null
        override var keys: SourceKeys? = SourceKeys(index = this@DefaultCatalogueRepositoryTest.keys.publicKey, packages = ByteArray(32))

        override suspend fun fetch(path: String): ByteArray {
            failure?.let { throw it }
            if (path == CatalogueSource.INDEX_SIGNATURE_PATH) {
                return signature ?: this@DefaultCatalogueRepositoryTest.keys.sign(files.getValue(CatalogueSource.INDEX_PATH).encodeToByteArray())
            }
            return files[path]?.encodeToByteArray() ?: throw FileNotFoundException(path)
        }

        override suspend fun openArchive(url: String): InputStream = throw FileNotFoundException(url)
    }

    private fun index(serial: Long, name: String = "App") =
        """
        { "schema": 1, "source": { "id": "test", "name": "Test" }, "serial": $serial,
          "packages": [{ "id": "a", "name": "$name", "details": "details/a.json",
            "versions": [{ "version": "1.0.0", "url": "https://example.invalid/a.nbtpkg", "size": 1, "sha256": "00",
                           "manifestSha256": "11" }] }] }
        """

    private val source = FakeSource().apply { files[CatalogueSource.INDEX_PATH] = index(serial = 5) }
    private val repository = DefaultCatalogueRepository(source)

    @Test
    fun `starts empty and holds the index after a refresh`() = runTest {
        assertNull(repository.catalogue.value.index)

        repository.refresh()

        val state = repository.catalogue.value
        assertEquals(listOf("a"), state.index?.packages?.map { it.id })
        assertFalse(state.refreshing)
        assertNull(state.error)
    }

    @Test
    fun `a failed first load reports the error`() = runTest {
        source.failure = IOException("no network")

        repository.refresh()

        val state = repository.catalogue.value
        assertNull(state.index)
        assertEquals("no network", state.error)
        assertFalse(state.refreshing)
    }

    @Test
    fun `a failed refresh keeps the index already held`() = runTest {
        repository.refresh()
        source.failure = IOException("no network")

        repository.refresh()

        val state = repository.catalogue.value
        assertNotNull(state.index)
        assertEquals("no network", state.error)
    }

    @Test
    fun `a later successful refresh clears the error and replaces the index`() = runTest {
        source.failure = IOException("no network")
        repository.refresh()
        source.failure = null
        source.files[CatalogueSource.INDEX_PATH] = index(serial = 6, name = "Renamed")

        repository.refresh()

        val state = repository.catalogue.value
        assertNull(state.error)
        assertEquals("Renamed", state.index?.packages?.single()?.name)
    }

    @Test
    fun `an index with a lower serial is rejected`() = runTest {
        repository.refresh()
        source.files[CatalogueSource.INDEX_PATH] = index(serial = 4, name = "Rolled back")

        repository.refresh()

        val state = repository.catalogue.value
        assertEquals(5L, state.index?.serial)
        assertEquals("App", state.index?.packages?.single()?.name)
        assertNotNull(state.error)
    }

    @Test
    fun `an index whose signature doesn't match is rejected, and the one held stays`() = runTest {
        repository.refresh()
        source.files[CatalogueSource.INDEX_PATH] = index(serial = 6, name = "Forged")
        source.signature = keys.sign(index(serial = 5).encodeToByteArray())

        repository.refresh()

        val state = repository.catalogue.value
        assertEquals("App", state.index?.packages?.single()?.name)
        assertNotNull(state.error)

        // A signature by another key is no better, nor is one of the wrong length.
        source.signature = TestKeys().sign(source.files.getValue(CatalogueSource.INDEX_PATH).encodeToByteArray())
        repository.refresh()
        assertEquals("App", repository.catalogue.value.index?.packages?.single()?.name)
        source.signature = ByteArray(10)
        repository.refresh()
        assertEquals("App", repository.catalogue.value.index?.packages?.single()?.name)
    }

    @Test
    fun `a source with no keys loads nothing`() = runTest {
        source.keys = null

        repository.refresh()

        assertNull(repository.catalogue.value.index)
        assertNotNull(repository.catalogue.value.error)
    }

    @Test
    fun `a malformed index reports an error`() = runTest {
        source.files[CatalogueSource.INDEX_PATH] = "{"

        repository.refresh()

        assertNull(repository.catalogue.value.index)
        assertNotNull(repository.catalogue.value.error)
    }

    @Test
    fun `details are fetched from the path the index gives`() = runTest {
        source.files["details/a.json"] = """{ "id": "a", "description": "About A" }"""
        repository.refresh()

        assertEquals("About A", repository.details("a").getOrThrow().description)
    }

    @Test
    fun `details fail for an unknown package, a missing file or the wrong package`() = runTest {
        repository.refresh()
        assertTrue(repository.details("unknown").isFailure)
        assertTrue(repository.details("a").isFailure)

        source.files["details/a.json"] = """{ "id": "b", "description": "About B" }"""
        assertTrue(repository.details("a").isFailure)
    }
}

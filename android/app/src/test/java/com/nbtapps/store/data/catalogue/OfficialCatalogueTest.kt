// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.crypto.sha256Hex
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * The official catalogue's address and keys, as the app holds them. The keys are held against their
 * fingerprints, which the catalogue repo's README lists, and against that repo's own files when it sits next to
 * this one (`nbtapps-catalogue` beside `nbtapps-mono`, or where the environment variable `NBT_CATALOGUE_REPO` names it).
 */
class OfficialCatalogueTest {

    // Unit tests run with the module folder as the working directory.
    private val catalogueRepo: File =
        (System.getenv("NBT_CATALOGUE_REPO")?.takeIf { it.isNotEmpty() }?.let(::File) ?: File("../../../nbtapps-catalogue")).absoluteFile.normalize()

    /** The first 16 hex digits of the key's SHA-256, in fours: what the package tool prints for a key. */
    private fun fingerprint(key: String): String =
        sha256Hex(requireNotNull(Hex.decodeOrNull(key, 32))).take(16).chunked(4).joinToString(" ")

    /** The one line of a key file that is not a comment. */
    private fun keyIn(name: String): String {
        val file = File(catalogueRepo, "keys/$name")
        assumeTrue("no catalogue repo next to this one", file.isFile)
        return file.readLines().map { it.trim() }.single { it.isNotEmpty() && !it.startsWith("#") }
    }

    @Test
    fun `the keys are the ones with the official fingerprints`() {
        assertEquals("f433 51c5 6418 466b", fingerprint(OfficialCatalogue.INDEX_KEY))
        assertEquals("d041 6126 bb84 2c92", fingerprint(OfficialCatalogue.PACKAGE_KEY))
    }

    @Test
    fun `the source is the published folder over https, with both keys`() {
        assertEquals("https://catalogue.nbtapps.com/v1/", OfficialCatalogue.ADDRESS)
        val keys = requireNotNull(OfficialCatalogue.source().keys)
        assertEquals(OfficialCatalogue.INDEX_KEY, Hex.encode(keys.index))
        assertEquals(OfficialCatalogue.PACKAGE_KEY, Hex.encode(keys.packages))
    }

    @Test
    fun `the keys are the catalogue repo's`() {
        assertEquals(keyIn("index.pub"), OfficialCatalogue.INDEX_KEY)
        assertEquals(keyIn("package.pub"), OfficialCatalogue.PACKAGE_KEY)
    }

    @Test
    fun `the address is the one the catalogue repo publishes at`() {
        val script = File(catalogueRepo, "tools/publish.py")
        assumeTrue("no catalogue repo next to this one", script.isFile)
        val published = OfficialCatalogue.ADDRESS.removeSuffix("/v1/")
        assertTrue("tools/publish.py does not name $published", script.readText().contains("\"$published\""))
    }
}

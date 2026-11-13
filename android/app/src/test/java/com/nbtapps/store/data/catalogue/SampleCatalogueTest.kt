// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.crypto.Ed25519
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.jobs.Resolver
import com.nbtapps.store.data.jobs.agentUpdate
import com.nbtapps.store.data.packages.PackageArchive
import com.nbtapps.store.data.packages.PackageFormatException
import com.nbtapps.store.model.RestartKind
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Checks the sample catalogue in the debug build's assets the way the app checks a real one, so it can't rot
 * unnoticed: the index against its signature, and every archive against the index entry that lists it.
 */
class SampleCatalogueTest {

    // Unit tests run with the module folder as the working directory.
    private val root = File("src/debug/assets/catalogue/official")

    private val indexKey = AgentVectors.hex(AssetCatalogueSource.TEST_INDEX_KEY)
    private val packageKey = AgentVectors.hex(AssetCatalogueSource.TEST_PACKAGE_KEY)

    private val rawIndex = File(root, "v1/" + CatalogueSource.INDEX_PATH).readBytes()
    private val index = CatalogueParser.parseIndex(rawIndex)

    @Test
    fun `the keys built into the debug build are the agent repo's test keys`() {
        assertArrayEquals(AgentVectors.indexKey, indexKey)
        assertArrayEquals(AgentVectors.packageKey, packageKey)
    }

    @Test
    fun `the index is signed by the index key, over its exact bytes`() {
        val signature = File(root, "v1/" + CatalogueSource.INDEX_SIGNATURE_PATH).readBytes()

        assertTrue(Ed25519.verify(indexKey, rawIndex, signature))
        assertFalse(Ed25519.verify(indexKey, rawIndex + '\n'.code.toByte(), signature))
        assertFalse("the package key must not sign the index", Ed25519.verify(packageKey, rawIndex, signature))
    }

    @Test
    fun `no package or version is dropped by the parser`() {
        val tree = Json.parseToJsonElement(rawIndex.decodeToString()).jsonObject
        val published = listOf("packages", "community").flatMap { tree.getValue(it).jsonArray }
            .sumOf { it.jsonObject.getValue("versions").jsonArray.size }

        assertTrue(index.packages.isNotEmpty())
        assertEquals(published, index.packages.sumOf { it.versions.size })
    }

    @Test
    fun `package ids are unique`() {
        val ids = index.packages.map { it.id }
        assertEquals(ids.distinct(), ids)
    }

    @Test
    fun `every package has a details file for itself`() {
        index.packages.forEach { pkg ->
            val path = requireNotNull(pkg.details) { "${pkg.id} has no details path" }
            val details = CatalogueParser.parseDetails(File(root, "v1/$path").readBytes())
            assertEquals(pkg.id, details.id)
            assertTrue("${pkg.id} has no description", details.description.isNotBlank())
        }
    }

    @Test
    fun `every listed archive is in the assets, verifies, and is what its entry says`() {
        val listed = HashSet<String>()
        for (pkg in index.packages) {
            for (version in pkg.versions) {
                val what = "${pkg.id} ${version.version}"
                assertTrue(what, version.url.startsWith("https://"))
                assertTrue(what, Hex.isSha256(version.sha256) && Hex.isSha256(version.manifestSha256))
                val name = version.url.substringAfterLast('/')
                listed += name

                // A community package's archive is checked with the catalogue's signature over its manifest.
                val signature = version.signature?.let { requireNotNull(Hex.decodeOrNull(it, 64)) { "$what has a bad signature" } }
                assertEquals(what, pkg.community, signature != null)
                val verified = PackageArchive.verify(File(root, "packages/$name"), listOf(packageKey), signature)
                verified.requireMatches(pkg.id, version)
                // What the index copies out of the manifest is still what the manifest says.
                assertEquals(what, pkg.name, verified.manifest.name)
                assertEquals(what, version.requires, verified.manifest.requires)
                assertEquals(what, version.depends, verified.manifest.depends)
                assertEquals(what, version.flags, verified.manifest.flags)
            }
        }
        assertEquals("archives nothing lists", listed, File(root, "packages").list().orEmpty().toSet())
    }

    @Test
    fun `every package can be resolved from the catalogue alone, for an agent that suits it`() {
        for (pkg in index.packages) {
            val newest = requireNotNull(pkg.latest)
            val resolution = Resolver.resolve(index, Resolver.Needed(pkg, newest), installed = emptyList(), agentVersion = null)
            assertEquals("${pkg.id} has a dependency the catalogue can't meet", emptyList<Any>(), resolution.unavailable)
            assertEquals(pkg.id, resolution.needed.last().pkg.id)
        }
    }

    /**
     * The catalogue screen's card for a newer agent can be seen in the debug build: the sample has the agent's
     * package in a version above the one a test bench runs, and packages whose newest versions wait for it.
     */
    @Test
    fun `the sample has an update of the agent for a bench agent, and packages that wait for it`() {
        // The sample's agent is a whole major version above the real one, whichever of its releases a bench runs.
        val bench = Holdings("bench", "Bench", live = true, agentVersion = "1.1.0", installed = emptyList(), restartNeeded = RestartKind.NONE, seenAt = 0)

        val update = requireNotNull(agentUpdate(index, bench)) { "no $AGENT_PACKAGE_ID above 1.1.0 in the sample" }

        assertEquals("2.0.0", update.version)
        // App Launcher asks for agent 2.0.0, and a community package for one that checks the catalogue's signature.
        assertEquals(listOf("App Launcher 0.3.0", "Trip Notes 0.2.0"), update.unblocks.map { it.label })
    }

    /**
     * An agent names the iDrive plugin's package when it is asked to start with the car and has nothing to do it.
     * The sample has that package, in a version a bench agent runs, so the way from the switch to the install
     * can be gone in the debug build.
     */
    @Test
    fun `the sample has the iDrive plugin's package, which a bench agent runs`() {
        val plugin = requireNotNull(index.packages.find { it.id == "org.nbtapp.plugin" }) { "no org.nbtapp.plugin in the sample" }

        assertTrue(Resolver.suits(requireNotNull(plugin.latest), "1.1.0"))
        assertEquals(RestartKind.HMI, PackageArchive.verify(File(root, "packages/org.nbtapp.plugin-1.0.0.nbtpkg"), listOf(packageKey)).manifest.reboot)
    }

    /**
     * The sample has a community package, as one comes into the catalogue: its author signed its archive with a key
     * of their own, and the index gives the catalogue's signature over the manifest. The app installs it with that
     * signature, and only on an agent that checks one.
     */
    @Test
    fun `the sample's community package installs with the catalogue's signature, on an agent from 1_2_0`() {
        val pkg = requireNotNull(index.packages.find { it.community }) { "no community package in the sample" }
        val newest = requireNotNull(pkg.latest)
        val file = File(root, "packages/${newest.url.substringAfterLast('/')}")
        val signature = requireNotNull(Hex.decodeOrNull(requireNotNull(newest.signature), 64))

        PackageArchive.verify(file, listOf(packageKey), signature).requireMatches(pkg.id, newest)
        assertThrows("its own manifest.sig is its author's", PackageFormatException::class.java) {
            PackageArchive.verify(file, listOf(packageKey))
        }
        assertFalse(pkg.id.startsWith("org.nbtapp."))
        assertFalse(Resolver.suits(newest, "1.1.0"))
        assertTrue(Resolver.suits(newest, "1.2.0"))
    }

    /** As the real agent's package is: an agent refuses a package of its own that places no launcher for the version. */
    @Test
    fun `the sample agent's package places a launcher and a starter, as the real one does`() {
        val agent = requireNotNull(index.packages.find { it.id == AGENT_PACKAGE_ID }?.latest)
        val manifest = PackageArchive.verify(File(root, "packages/$AGENT_PACKAGE_ID-${agent.version}.nbtpkg"), listOf(packageKey)).manifest

        assertEquals(
            listOf("\${app}/nbtagent", "\${app}/nbtctl", "\${app}/nbtlaunch.sh", "\${app}/nbtstart.sh"),
            manifest.files.map { it.dest },
        )
    }
}

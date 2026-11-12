// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageDependency
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ResolverTest {

    private fun version(version: String, agent: String? = null, vararg depends: Pair<String, String?>) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = "00",
        manifestSha256 = "11",
        requires = PackageRequirements(agent),
        depends = depends.map { PackageDependency(it.first, it.second) },
    )

    /** Versions newest first, as the catalogue parser leaves them. */
    private fun pkg(id: String, vararg versions: PackageVersion) = CataloguePackage(id = id, name = id, versions = versions.toList())

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test"),
        serial = 1,
        packages = listOf(
            pkg("lib", version("3.0.0"), version("2.1.0"), version("2.0.0"), version("1.0.0")),
            pkg("ui", version("0.3.0", ">=1.1.0", "lib" to ">=2.0.0 <3.0.0"), version("0.2.0", ">=1.0.0", "lib" to ">=2.0.0 <3.0.0")),
            pkg("app", version("1.0.0", null, "ui" to ">=0.2.0", "lib" to ">=2.0.0 <3.0.0")),
            pkg("lonely", version("1.0.0", null, "nowhere" to null)),
            pkg("a", version("1.0.0", null, "b" to null)),
            pkg("b", version("1.0.0", null, "a" to null)),
        ),
    )

    private fun resolve(id: String, installed: List<InstalledPackage> = emptyList(), agent: String? = null): Resolver.Resolution {
        val pkg = index.packages.first { it.id == id }
        return Resolver.resolve(index, Resolver.Needed(pkg, pkg.versions.first()), installed, agent)
    }

    private fun Resolver.Resolution.names() = needed.map { "${it.pkg.id} ${it.version.version}" }

    private fun installed(id: String, version: String) = InstalledPackage(id, id, version)

    @Test
    fun `a package with no dependencies needs only itself`() {
        assertEquals(listOf("lib 3.0.0"), resolve("lib").names())
    }

    @Test
    fun `dependencies come ahead of what needs them, each at the highest version in range`() {
        val resolution = resolve("app")
        assertEquals(listOf("lib 2.1.0", "ui 0.3.0", "app 1.0.0"), resolution.names())
        assertTrue(resolution.unavailable.isEmpty())
    }

    @Test
    fun `a version the head unit's agent is too old for is passed over`() {
        assertEquals(listOf("lib 2.1.0", "ui 0.2.0", "app 1.0.0"), resolve("app", agent = "1.0.0").names())
        assertEquals(listOf("lib 2.1.0", "ui 0.3.0", "app 1.0.0"), resolve("app", agent = "1.1.0").names())
    }

    @Test
    fun `what the head unit has in range is left alone`() {
        assertEquals(listOf("ui 0.3.0", "app 1.0.0"), resolve("app", listOf(installed("lib", "2.0.0"))).names())
        assertEquals(listOf("app 1.0.0"), resolve("app", listOf(installed("lib", "2.0.0"), installed("ui", "0.2.0"))).names())
    }

    @Test
    fun `an installed version outside the range is replaced, but never by a lower one`() {
        assertEquals(listOf("lib 2.1.0", "ui 0.3.0", "app 1.0.0"), resolve("app", listOf(installed("lib", "1.0.0"))).names())

        // lib 3.0.0 is installed and nothing in range is as high: the agent would refuse the downgrade.
        val blocked = resolve("app", listOf(installed("lib", "3.0.0")))
        assertEquals(listOf("ui 0.3.0", "app 1.0.0"), blocked.names())
        assertEquals(listOf("lib", "lib"), blocked.unavailable.map { it.id })
    }

    @Test
    fun `a dependency the catalogue does not have is reported, with who asked for it`() {
        val resolution = resolve("lonely")
        assertEquals(listOf("lonely 1.0.0"), resolution.names())
        assertEquals("nowhere", resolution.unavailable.single().id)
        assertEquals("lonely", resolution.unavailable.single().requiredBy)
    }

    @Test
    fun `a cycle ends`() {
        assertEquals(listOf("b 1.0.0", "a 1.0.0"), resolve("a").names())
    }
}

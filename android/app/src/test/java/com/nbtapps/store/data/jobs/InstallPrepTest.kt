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
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** What the job screen takes for an install before it asks the agent ([installPrep]), and where it stops instead. */
class InstallPrepTest {

    private fun version(version: String, agent: String? = null, vararg depends: Pair<String, String?>) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = "00",
        manifestSha256 = "11",
        requires = PackageRequirements(agent),
        depends = depends.map { PackageDependency(it.first, it.second) },
    )

    private fun pkg(id: String, name: String, stable: List<PackageVersion>, prereleases: List<PackageVersion> = emptyList()) =
        CataloguePackage(id = id, name = name, versions = stable, prereleases = prereleases)

    private fun has(id: String, version: String) = InstalledPackage(id, id, version)

    /** Hello's pre-release needs both a pre-release agent and a pre-release runtime. */
    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test"),
        serial = 1,
        packages = listOf(
            pkg(AGENT_PACKAGE_ID, "NBTApp agent", listOf(version("2.0.0", ">=1.1.0")), listOf(version("2.1.0-beta.1", ">=1.1.0"))),
            pkg(
                "hello", "Hello NBT",
                listOf(version("1.2.0", ">=1.0.0", "runtime" to ">=2.0.0 <3.0.0")),
                listOf(version("1.3.0-beta.1", ">=2.1.0-beta.1", "runtime" to ">=2.2.0-beta.1 <3.0.0")),
            ),
            pkg("runtime", "NBT Runtime", listOf(version("2.1.0")), listOf(version("2.2.0-beta.1"))),
        ),
    )
    private val hello = index.packages.first { it.id == "hello" }
    private val beta = hello.prereleases.single()
    private val installed = listOf(has("hello", "1.2.0"), has("runtime", "2.1.0"))

    private fun InstallPrep.names() = needed.map { "${it.pkg.id} ${it.version.version}" }

    @Test
    fun `a job for a version that waits for another package's pre-release stops before anything is downloaded or planned`() {
        // The agent updated to its pre-release, Hello's switch on and the runtime's off: the job is opened all the same.
        val prep = installPrep(index, hello, beta, installed, "2.1.0-beta.1", setOf(AGENT_PACKAGE_ID, "hello"))

        assertEquals(
            "Needs NBT Runtime 2.2.0-beta.1, which is a pre-release. To install this version, turn on pre-release versions of NBT Runtime first.",
            prep.waiting,
        )
        assertTrue(prep.needed.isEmpty())
    }

    @Test
    fun `a job for a version that a pre-release agent runs stops too, while the agent's switch is off`() {
        val prep = installPrep(index, hello, beta, installed, "2.0.0", setOf("hello"))

        assertEquals(
            "Needs NBT Runtime 2.2.0-beta.1 and NBTApp agent 2.1.0-beta.1, which are pre-releases. To install this version, " +
                "turn on pre-release versions of NBT Runtime and NBTApp agent first.",
            prep.waiting,
        )
        assertTrue(prep.needed.isEmpty())
    }

    @Test
    fun `with every switch it needs on, the job offers the agent what the version takes`() {
        val prep = installPrep(index, hello, beta, installed, "2.1.0-beta.1", setOf(AGENT_PACKAGE_ID, "hello", "runtime"))

        assertNull(prep.waiting)
        assertEquals(listOf("runtime 2.2.0-beta.1", "hello 1.3.0-beta.1"), prep.names())
        // A stable version with every switch off, the same.
        val stable = installPrep(index, hello, hello.versions.single(), emptyList(), "2.0.0", emptySet())
        assertNull(stable.waiting)
        assertEquals(listOf("runtime 2.1.0", "hello 1.2.0"), stable.names())
    }

    @Test
    fun `what stops a job names each package to switch on`() {
        val one = PrereleaseAsk("runtime", "NBT Runtime", "2.2.0-beta.1")
        val two = PrereleaseAsk("lib", "Lib", "1.0.0-rc.1")
        val three = PrereleaseAsk(AGENT_PACKAGE_ID, "NBTApp agent", "2.1.0-beta.1")

        assertEquals(
            "Needs NBT Runtime 2.2.0-beta.1, Lib 1.0.0-rc.1 and NBTApp agent 2.1.0-beta.1, which are pre-releases. To install " +
                "this version, turn on pre-release versions of NBT Runtime, Lib and NBTApp agent first.",
            waitingFor(listOf(one, two, three)),
        )
    }
}

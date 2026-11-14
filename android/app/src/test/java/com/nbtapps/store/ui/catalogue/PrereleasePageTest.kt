// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.jobs.PackageStatus
import com.nbtapps.store.data.jobs.PrereleaseAsk
import com.nbtapps.store.data.jobs.packageStatus
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.ChangelogEntry
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageDependency
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.RestartKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What a package's page ([cataloguePackageUiState]) and the list's rows show of pre-release versions: the switch,
 * the labels, the version that waits for the switch, and the questions put where another package's pre-release
 * is needed.
 */
class PrereleasePageTest {

    private fun version(version: String, agent: String? = ">=1.0.0", vararg depends: Pair<String, String?>) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = version.padEnd(8, '0'),
        manifestSha256 = "11",
        requires = PackageRequirements(agent),
        depends = depends.map { PackageDependency(it.first, it.second) },
    )

    private fun pkg(id: String, name: String, stable: List<PackageVersion>, prereleases: List<PackageVersion> = emptyList()) =
        CataloguePackage(id = id, name = name, versions = stable, prereleases = prereleases)

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test source"),
        serial = 1,
        packages = listOf(
            pkg(AGENT_PACKAGE_ID, "NBTApp agent", listOf(version("1.1.0")), listOf(version("1.2.0-rc.1"))),
            pkg("lib", "Runtime", listOf(version("2.0.0")), listOf(version("2.1.0-rc.1"))),
            pkg(
                "radio", "Radio",
                listOf(version("1.0.0", ">=1.0.0", "lib" to ">=2.0.0")),
                listOf(version("1.1.0-rc.1", ">=1.0.0", "lib" to ">=2.1.0-rc.1")),
            ),
            // Its only version needs an agent that only a pre-release agent is.
            pkg("next", "Next Thing", emptyList(), listOf(version("0.1.0-rc.1", ">=1.2.0-rc.1"))),
            pkg("early", "Early Bird", emptyList(), listOf(version("0.1.0-beta.1"))),
            pkg("plain", "Plain", listOf(version("1.0.0"))),
        ),
    )

    private fun held(agent: String, vararg installed: InstalledPackage, live: Boolean = true) =
        Holdings("car", "Car", live = live, agentVersion = agent, installed = installed.toList(), restartNeeded = RestartKind.NONE, seenAt = 1)

    private fun has(id: String, version: String) = InstalledPackage(id, id, version)

    private fun stored(version: PackageVersion) = StoredPackage("x", "x", version.version, version.sha256, 1, "11", "00", "00")

    private fun page(
        id: String,
        switchedOn: Set<String> = emptySet(),
        head: Holdings? = held("1.1.0"),
        stored: List<StoredPackage> = emptyList(),
        details: DetailsState = DetailsState.Loading,
    ) = cataloguePackageUiState(CatalogueState(index = index), id, details, stored, emptyMap(), head, switchedOn)

    // ---- the switch, and what it offers ----

    @Test
    fun `the switch is shown for a package with pre-releases, or one that is switched on, and not otherwise`() {
        assertTrue(page("radio").showsPrereleaseSwitch)
        assertFalse(page("radio").prereleaseOn)
        assertTrue(page("radio", setOf("radio")).prereleaseOn)
        assertFalse(page("plain").showsPrereleaseSwitch)
        // Switched on once and its pre-releases since gone: still shown, so that it can be turned off.
        assertTrue(page("plain", setOf("plain")).showsPrereleaseSwitch)
    }

    @Test
    fun `with the switch off the page offers and describes the stable version, and with it on the newest of either`() {
        val off = page("radio")
        assertEquals("1.0.0", off.get?.version?.version)
        assertFalse(off.get!!.offeredPrerelease)
        assertEquals("1.0.0", off.newest?.version)
        assertEquals(listOf(">=2.0.0"), off.dependencies.map { it.versionRange })

        val on = page("radio", setOf("radio", "lib"))
        assertEquals("1.1.0-rc.1", on.get?.version?.version)
        assertTrue(on.get!!.offeredPrerelease)
        assertEquals("1.1.0-rc.1", on.newest?.version)
        assertEquals(listOf(">=2.1.0-rc.1"), on.dependencies.map { it.versionRange })
        assertEquals(listOf("lib 2.1.0-rc.1", "radio 1.1.0-rc.1"), on.get!!.toDownload.map { "${it.pkg.id} ${it.version.version}" })
    }

    @Test
    fun `an installed pre-release is marked, and with the switch off is up to date`() {
        val get = requireNotNull(page("radio", head = held("1.1.0", has("radio", "1.1.0-rc.1"))).get)

        assertTrue(get.installedPrerelease)
        assertTrue(get.upToDate)
        assertFalse(get.canInstall)
    }

    // ---- a package out only in pre-releases ----

    @Test
    fun `a package out only in pre-releases waits for its switch, and offers nothing to download until then`() {
        val off = page("early")
        val get = requireNotNull(off.get)
        assertTrue(get.needsSwitch)
        assertEquals("0.1.0-beta.1", get.version.version)
        assertTrue(get.toDownload.isEmpty())
        assertNull(get.incompatible)
        assertFalse(get.canInstall)
        assertEquals("0.1.0-beta.1", off.newest?.version)
        assertTrue(off.showsPrereleaseSwitch)

        // Switched on, it is offered as any other package: download first, then install.
        val on = requireNotNull(page("early", setOf("early")).get)
        assertFalse(on.needsSwitch)
        assertEquals(listOf("early"), on.toDownload.map { it.pkg.id })
        val ready = requireNotNull(page("early", setOf("early"), stored = listOf(on.version).map(::stored)).get)
        assertTrue(ready.canInstall)

        // On the head unit already with the switch off: up to date.
        val there = requireNotNull(page("early", head = held("1.1.0", has("early", "0.1.0-beta.1"))).get)
        assertTrue(there.upToDate)
        assertTrue(there.installedPrerelease)
    }

    // ---- what another package's pre-release is needed for ----

    @Test
    fun `a dependency only a pre-release meets is asked for by name, and blocks the install until then`() {
        // Radio's pre-release is switched on, the runtime it needs is not.
        val get = requireNotNull(page("radio", setOf("radio"), stored = listOf(stored(version("1.1.0-rc.1", ">=1.0.0")))).get)

        assertEquals(listOf(PrereleaseAsk("lib", "Runtime", "2.1.0-rc.1")), get.asks)
        assertNull(get.incompatible)
        assertTrue(get.toDownload.isEmpty())
        assertFalse(get.canInstall)
    }

    @Test
    fun `a version only a pre-release agent runs asks for the agent's switch, in place of the agent message`() {
        val off = requireNotNull(page("next", setOf("next")).get)
        assertEquals("0.1.0-rc.1", off.version.version)
        assertEquals(listOf(PrereleaseAsk(AGENT_PACKAGE_ID, "NBTApp agent", "1.2.0-rc.1")), off.asks)
        assertNull(off.incompatible)
        assertFalse(off.canInstall)

        // With the agent's switch on, the agent is updated first, as for any agent that is too old.
        val on = requireNotNull(page("next", setOf("next", AGENT_PACKAGE_ID)).get)
        assertTrue(on.asks.isEmpty())
        assertEquals("Needs agent 1.2.0-rc.1 or later; this head unit's agent is 1.1.0.", on.incompatible)
        // And on the pre-release agent nothing stands in the way.
        val there = requireNotNull(page("next", setOf("next"), head = held("1.2.0-rc.1")).get)
        assertTrue(there.asks.isEmpty())
        assertNull(there.incompatible)
    }

    @Test
    fun `a dependency only a pre-release agent runs asks for the agent's switch, and is not reported missing`() {
        val deps = index.copy(
            packages = index.packages + listOf(
                pkg("fast", "Fast Lib", listOf(version("1.0.0", ">=1.2.0-rc.1"))),
                pkg("usesfast", "Uses Fast", listOf(version("1.0.0", ">=1.0.0", "fast" to ">=1.0.0"))),
                pkg("steady", "Steady Lib", listOf(version("1.0.0", ">=1.1.0"))),
                pkg("usessteady", "Uses Steady", listOf(version("1.0.0", ">=1.0.0", "steady" to ">=1.0.0"))),
            ),
        )
        fun get(id: String) = requireNotNull(cataloguePackageUiState(CatalogueState(index = deps), id, DetailsState.Loading, emptyList(), emptyMap(), held("1.0.2"), emptySet()).get)

        val fast = get("usesfast")
        assertEquals(listOf(PrereleaseAsk(AGENT_PACKAGE_ID, "NBTApp agent", "1.2.0-rc.1")), fast.asks)
        assertNull(fast.incompatible)
        assertFalse(fast.canInstall)
        // Where a newer stable agent would run it, as before: the catalogue has nothing that fits this agent.
        val steady = get("usessteady")
        assertTrue(steady.asks.isEmpty())
        assertEquals("Needs steady, which this catalogue doesn't have in a version that fits.", steady.incompatible)
    }

    @Test
    fun `with every switch off a package with pre-releases is offered as it was before`() {
        val stripped = index.copy(packages = index.packages.map { it.copy(prereleases = emptyList()) })
        for (pkg in index.packages.filterNot { it.prereleaseOnly }) {
            val with = requireNotNull(page(pkg.id).get)
            val without = requireNotNull(
                cataloguePackageUiState(CatalogueState(index = stripped), pkg.id, DetailsState.Loading, emptyList(), emptyMap(), held("1.1.0"), emptySet()).get,
            )
            // What is to download names the package itself, which differs by its pre-releases: compared by what is downloaded.
            assertEquals(pkg.id, without.copy(toDownload = emptyList()), with.copy(toDownload = emptyList()))
            assertEquals(pkg.id, without.toDownload.map { it.pkg.id to it.version }, with.toDownload.map { it.pkg.id to it.version })
        }
    }

    // ---- the questions ----

    @Test
    fun `turning on the agent's pre-releases warns of the way back, and other packages' do not`() {
        val agent = prereleaseWarning(PrereleaseAsk(AGENT_PACKAGE_ID, "NBTApp agent", "1.2.0-rc.1"))
        val other = prereleaseWarning(PrereleaseAsk("radio", "Radio", "1.1.0-rc.1"))

        assertTrue(agent.contains("USB stick"))
        assertFalse(other.contains("USB stick"))
        assertTrue(other.contains("every head unit"))
        assertEquals("Turn on pre-releases of Radio?", prereleaseQuestion(PrereleaseAsk("radio", "Radio", "1.1.0-rc.1")))
        assertTrue(prereleaseSwitchText(on = false, agent = true).contains("USB stick"))
        assertFalse(prereleaseSwitchText(on = false, agent = false).contains("USB stick"))
    }

    // ---- versions and what is new ----

    @Test
    fun `every version is listed newest first, the pre-releases marked`() {
        val radio = index.packages.first { it.id == "radio" }

        assertEquals(listOf("1.1.0-rc.1" to true, "1.0.0" to false), allVersions(radio).map { it.version.version to it.prerelease })
    }

    @Test
    fun `the notes of pre-releases are among the rest, newest first and marked, until their version is stable`() {
        val radio = index.packages.first { it.id == "radio" }
        val details = PackageDetails(
            id = "radio",
            changelog = listOf(ChangelogEntry("1.0.0", listOf("First")), ChangelogEntry("0.9.0", emptyList())),
            prereleaseChangelog = listOf(ChangelogEntry("1.1.0-rc.1", listOf("Try this")), ChangelogEntry("1.0.0", listOf("Was a pre-release"))),
        )

        val rows = changelog(details, radio)
        assertEquals(listOf("1.1.0-rc.1" to true, "1.0.0" to false, "1.0.0" to false), rows.map { it.entry.version to it.prerelease })

        // Without pre-release notes, as the file has them.
        val plain = changelog(details.copy(prereleaseChangelog = emptyList()), radio)
        assertEquals(listOf("1.0.0"), plain.map { it.entry.version })
        assertFalse(plain.single().prerelease)
    }

    // ---- the list's rows ----

    private fun status(
        offeredPrerelease: Boolean = false,
        installed: String? = null,
        installedPrerelease: Boolean = false,
        needsSwitch: Boolean = false,
        offered: String = "1.1.0",
    ) = PackageStatus(
        version(offered),
        suits = true,
        installed = installed,
        heldBack = null,
        offeredPrerelease = offeredPrerelease,
        installedPrerelease = installedPrerelease,
        needsSwitch = needsSwitch,
    )

    @Test
    fun `a row marks the version it names when that is a pre-release`() {
        // On offer.
        assertEquals(PRERELEASE, rowPrereleaseLabel(status(offeredPrerelease = true)))
        assertNull(rowPrereleaseLabel(status()))
        // An update to a pre-release, and one from a pre-release to a stable version.
        assertEquals(PRERELEASE, rowPrereleaseLabel(status(offeredPrerelease = true, installed = "1.0.0")))
        assertNull(rowPrereleaseLabel(status(installed = "1.1.0-rc.1", installedPrerelease = true)))
        // Installed and up to date.
        assertEquals(PRERELEASE, rowPrereleaseLabel(status(installed = "1.2.0-rc.1", installedPrerelease = true)))
        assertNull(rowPrereleaseLabel(status(installed = "1.1.0")))
        // Out only in pre-releases.
        assertEquals(PRERELEASE_ONLY, rowPrereleaseLabel(status(offeredPrerelease = true, needsSwitch = true)))
        assertEquals(PRERELEASE, rowPrereleaseLabel(status(offeredPrerelease = true, needsSwitch = true, installed = "0.1.0-beta.1", installedPrerelease = true)))
    }

    @Test
    fun `the list counts an update to a pre-release only where the package is switched on`() {
        val head = held("1.1.0", has("radio", "1.0.0"), has("lib", "2.0.0"))
        val off = catalogueUiState(CatalogueState(index = index), head, "", CatalogueFilter.All, HeadUnitRefresh())
        val on = catalogueUiState(CatalogueState(index = index), head, "", CatalogueFilter.All, HeadUnitRefresh(), setOf("lib"))

        assertEquals(0, off.updateCount)
        assertEquals(1, on.updateCount)
        assertEquals(listOf("Runtime"), on.sections.first { it.kind == CatalogueSection.Kind.UPDATES }.rows.map { it.pkg.name })
        // A package out only in pre-releases is listed for everyone.
        assertTrue(off.packages.any { it.id == "early" })

        // The agent's card offers its pre-release, marked, only with its switch on.
        val older = held("1.0.2")
        val stable = requireNotNull(catalogueUiState(CatalogueState(index = index), older, "", CatalogueFilter.All, HeadUnitRefresh()).agentUpdate)
        assertEquals("1.1.0", stable.version)
        assertFalse(stable.prerelease)
        val agent = catalogueUiState(CatalogueState(index = index), older, "", CatalogueFilter.All, HeadUnitRefresh(), setOf(AGENT_PACKAGE_ID))
        assertEquals("1.2.0-rc.1", agent.agentUpdate?.version)
        assertTrue(agent.agentUpdate!!.prerelease)
    }

    // ---- the list and the page agree ----

    private fun list(head: Holdings?, switchedOn: Set<String>, filter: CatalogueFilter = CatalogueFilter.All) =
        catalogueUiState(CatalogueState(index = index), head, "", filter, HeadUnitRefresh(), switchedOn)

    private fun CatalogueUiState.row(id: String) = sections.flatMap { it.rows }.single { it.pkg.id == id }

    @Test
    fun `an update that waits for another package's pre-release is no update in the list, and its row says why`() {
        val head = held("1.1.0", has("radio", "1.0.0"), has("lib", "2.0.0"))
        // Radio's pre-release is switched on, the runtime's is not: the page asks for that, and will not update.
        val page = requireNotNull(page("radio", setOf("radio"), head).get)
        assertEquals(listOf(PrereleaseAsk("lib", "Runtime", "2.1.0-rc.1")), page.asks)
        assertFalse(page.canInstall)

        val off = list(head, setOf("radio"))
        val row = off.row("radio")
        assertEquals(page.asks, row.status!!.asks)
        assertEquals(page.version, row.status!!.offered)
        assertFalse(row.status!!.updateAvailable)
        assertEquals("1.1.0-rc.1 needs Runtime 2.1.0-rc.1, a pre-release", row.status!!.note())
        assertEquals(0, off.updateCount)
        assertTrue(off.sections.none { it.kind == CatalogueSection.Kind.UPDATES })
        assertEquals("No updates for Car.", list(head, setOf("radio"), CatalogueFilter.Updates).emptyMessage())

        // With the runtime switched on as well both are updates, and the note has gone.
        val on = list(head, setOf("radio", "lib"))
        assertEquals(2, on.updateCount)
        assertEquals(listOf("Radio", "Runtime"), on.sections.first { it.kind == CatalogueSection.Kind.UPDATES }.rows.map { it.pkg.name })
        assertNull(on.row("radio").status!!.note())
    }

    @Test
    fun `a package that is not installed notes the pre-release it waits for, as its page asks for it`() {
        val head = held("1.1.0")
        val page = requireNotNull(page("radio", setOf("radio"), head).get)
        val row = list(head, setOf("radio")).row("radio")

        assertEquals(page.asks, row.status!!.asks)
        assertEquals("1.1.0-rc.1 needs Runtime 2.1.0-rc.1, a pre-release", row.status!!.note())
        // With no head unit known the page asks all the same, and so does the row.
        assertEquals(page("radio", setOf("radio"), head = null).get!!.asks, list(null, setOf("radio")).row("radio").status!!.asks)
    }

    @Test
    fun `a version that waits for a pre-release agent is no update, and its row names that agent`() {
        val head = held("1.1.0", has("next", "0.1.0-beta.1"))
        val page = requireNotNull(page("next", setOf("next"), head).get)
        val off = list(head, setOf("next"))
        val row = off.row("next")

        assertEquals(listOf(PrereleaseAsk(AGENT_PACKAGE_ID, "NBTApp agent", "1.2.0-rc.1")), row.status!!.asks)
        assertEquals(page.asks, row.status!!.asks)
        assertFalse(row.status!!.updateAvailable)
        assertEquals(0, off.updateCount)
        assertEquals("0.1.0-rc.1 needs NBTApp agent 1.2.0-rc.1, a pre-release", row.status!!.note())
        // Not installed, the same.
        assertEquals("0.1.0-rc.1 needs NBTApp agent 1.2.0-rc.1, a pre-release", list(held("1.1.0"), setOf("next")).row("next").status!!.note())
    }

    @Test
    fun `whatever is switched on, a row counts an update exactly where its page would install one`() {
        val ids = listOf("radio", "lib", "next", "early", AGENT_PACKAGE_ID)
        val heads = listOf(
            held("1.1.0"),
            held("1.1.0", has("radio", "1.0.0"), has("lib", "2.0.0")),
            held("1.1.0", has("next", "0.1.0-beta.1"), has("early", "0.1.0-alpha.1")),
            held("1.2.0-rc.1", has("radio", "1.0.0"), has("lib", "2.1.0-rc.1")),
        )
        for (mask in 0 until (1 shl ids.size)) {
            val switchedOn = ids.filterIndexed { i, _ -> mask and (1 shl i) != 0 }.toSet()
            for (head in heads) {
                val listed = list(head, switchedOn)
                for (pkg in index.packages) {
                    val what = "${pkg.id} with $switchedOn on ${head.agentVersion} ${head.installed}"
                    val status = requireNotNull(listed.row(pkg.id).status)
                    val first = requireNotNull(page(pkg.id, switchedOn, head).get)
                    // Everything it needs on the phone already: what stands in the way is then the head unit's alone.
                    val ready = requireNotNull(page(pkg.id, switchedOn, head, stored = first.toDownload.map { stored(it.version) }).get)
                    assertEquals(what, first.asks, status.asks)
                    assertEquals(what, first.version, status.offered)
                    assertEquals(what, ready.canInstall && ready.installedVersion != null, status.updateAvailable)
                }
                assertEquals(listed.packages.count { listed.row(it.id).status!!.updateAvailable }, listed.updateCount)
            }
        }
    }
}

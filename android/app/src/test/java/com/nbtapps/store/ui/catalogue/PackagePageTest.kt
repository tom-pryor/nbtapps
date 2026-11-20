// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.ChangelogEntry
import com.nbtapps.store.model.InstalledPackage
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
 * What a package's page ([cataloguePackageUiState]) offers of uninstalling it, what it greys while a job that needs
 * the phone is running, and how it shows its download, its header's marks, About, what is new and every version.
 */
class PackagePageTest {

    private fun version(version: String, agent: String? = ">=1.0.0", released: String? = null) = PackageVersion(
        version = version,
        released = released,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = version.padEnd(8, '0'),
        manifestSha256 = "11",
        requires = PackageRequirements(agent),
    )

    private fun pkg(id: String, name: String, stable: List<PackageVersion>, prereleases: List<PackageVersion> = emptyList()) =
        CataloguePackage(id = id, name = name, versions = stable, prereleases = prereleases)

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test source"),
        serial = 1,
        packages = listOf(
            pkg(AGENT_PACKAGE_ID, "NBTApp agent", listOf(version("1.1.0"), version("1.0.2"))),
            pkg("plain", "Plain", listOf(version("1.0.0"), version("0.9.0"))),
            pkg("early", "Early Bird", emptyList(), listOf(version("0.1.0-beta.1"))),
            // Its only version needs an agent newer than the head unit's.
            pkg("later", "Later", listOf(version("1.0.0", ">=2.0.0"))),
        ),
    )

    private fun held(agent: String, vararg installed: InstalledPackage, live: Boolean = true) =
        Holdings("car", "Test Coupe", live = live, agentVersion = agent, installed = installed.toList(), restartNeeded = RestartKind.NONE, seenAt = 1)

    private fun has(id: String, version: String) = InstalledPackage(id, id, version)

    private fun stored(version: String) = StoredPackage("x", "x", version, version.padEnd(8, '0'), 1, "11", "00", "00")

    private fun get(
        id: String,
        head: Holdings? = held("1.1.0"),
        stored: List<String> = emptyList(),
        busy: Boolean = false,
        switchedOn: Set<String> = emptySet(),
    ) = requireNotNull(
        cataloguePackageUiState(
            CatalogueState(index = index), id, DetailsState.Loading, stored.map(::stored), emptyMap(), head, switchedOn, busy,
        ).get,
    )

    // ---- uninstall ----

    @Test
    fun `the rules for uninstalling apply in order, the first that fits deciding`() {
        assertEquals(UninstallState.None, uninstallState("plain", installed = false, connected = false, busy = true))
        assertEquals(UninstallState.Blocked(UninstallState.Reason.AGENT), uninstallState(AGENT_PACKAGE_ID, installed = true, connected = false, busy = true))
        assertEquals(UninstallState.Blocked(UninstallState.Reason.NO_LINK), uninstallState("plain", installed = true, connected = false, busy = true))
        assertEquals(UninstallState.Blocked(UninstallState.Reason.BUSY), uninstallState("plain", installed = true, connected = true, busy = true))
        assertEquals(UninstallState.Ready, uninstallState("plain", installed = true, connected = true, busy = false))
    }

    @Test
    fun `a package that is not installed has no Uninstall and says nothing of it`() {
        val get = get("plain")

        assertEquals(UninstallState.None, get.uninstall)
        assertFalse(get.uninstall.showsButton)
        assertNull(uninstallLine(get))
        assertNull(jobLine(get))
    }

    @Test
    fun `an installed package on a head unit the app is connected to can be uninstalled`() {
        val get = get("plain", held("1.1.0", has("plain", "1.0.0")))

        assertEquals(UninstallState.Ready, get.uninstall)
        assertTrue(get.uninstall.showsButton)
        assertNull(uninstallLine(get))
        assertNull(jobLine(get))
    }

    @Test
    fun `the agent has no Uninstall, and a line says why`() {
        val get = get(AGENT_PACKAGE_ID, held("1.1.0"))

        assertEquals("1.1.0", get.installedVersion)
        assertEquals(UninstallState.Blocked(UninstallState.Reason.AGENT), get.uninstall)
        assertFalse(get.uninstall.showsButton)
        assertEquals("The agent can't be uninstalled. Everything else on the head unit needs it.", uninstallLine(get))
    }

    @Test
    fun `with no link Uninstall is greyed and says to connect, in the update's line where there is one`() {
        // Up to date: a line of its own.
        val upToDate = get("plain", held("1.1.0", has("plain", "1.0.0"), live = false))
        assertEquals(UninstallState.Blocked(UninstallState.Reason.NO_LINK), upToDate.uninstall)
        assertTrue(upToDate.uninstall.showsButton)
        assertNull(connectLine(upToDate))
        assertEquals("Connect to Test Coupe to uninstall it, from its name at the top of this screen.", uninstallLine(upToDate))

        // An update waiting: the one line says both.
        val update = get("plain", held("1.1.0", has("plain", "0.9.0"), live = false), stored = listOf("1.0.0"))
        assertEquals(
            "Connect to Test Coupe to update or uninstall it, from its name at the top of this screen. The download needs no car.",
            connectLine(update),
        )
        assertNull(uninstallLine(update))

        // Not installed, and the agent, which has no Uninstall: as before.
        assertEquals(
            "Connect to Test Coupe to install it, from its name at the top of this screen. The download needs no car.",
            connectLine(get("plain", held("1.1.0", live = false))),
        )
        assertEquals(
            "Connect to Test Coupe to update it, from its name at the top of this screen. The download needs no car.",
            connectLine(get(AGENT_PACKAGE_ID, held("1.0.2", live = false))),
        )
        // With a link there is no such line.
        assertNull(connectLine(get("plain", held("1.1.0", has("plain", "0.9.0")))))
    }

    @Test
    fun `a package that cannot go on the head unit can still be uninstalled, with the connect line of its own`() {
        val get = get("later", held("1.1.0", has("later", "0.5.0"), live = false))

        assertTrue(get.incompatible != null)
        assertNull(connectLine(get))
        assertEquals("Connect to Test Coupe to uninstall it, from its name at the top of this screen.", uninstallLine(get))
    }

    @Test
    fun `an installed package out only in pre-releases has Uninstall as well`() {
        val get = get("early", held("1.1.0", has("early", "0.1.0-beta.1")))

        assertTrue(get.needsSwitch)
        assertEquals(UninstallState.Ready, get.uninstall)
        val away = get("early", held("1.1.0", has("early", "0.1.0-beta.1"), live = false))
        assertEquals("Connect to Test Coupe to uninstall it, from its name at the top of this screen.", uninstallLine(away))
    }

    // ---- while a job runs ----

    @Test
    fun `while a job runs Install is greyed and the line says so, and the download is still offered`() {
        val ready = get("plain", stored = listOf("1.0.0"), busy = true)
        assertTrue(ready.readyToInstall)
        assertFalse(ready.canInstall)
        assertEquals("A job is running on a head unit. Install once it has finished.", jobLine(ready))
        assertTrue(get("plain", stored = listOf("1.0.0")).canInstall)

        // Not on the phone yet: the download is what to do, and the job greys nothing that was not greyed already.
        val toFetch = get("plain", busy = true)
        assertEquals(listOf("plain"), toFetch.toDownload.map { it.pkg.id })
        assertNull(jobLine(toFetch))
    }

    @Test
    fun `while a job runs Update and Uninstall are greyed, and one line names what is`() {
        val both = get("plain", held("1.1.0", has("plain", "0.9.0")), stored = listOf("1.0.0"), busy = true)
        assertFalse(both.canInstall)
        assertEquals(UninstallState.Blocked(UninstallState.Reason.BUSY), both.uninstall)
        assertTrue(both.uninstall.showsButton)
        assertNull(uninstallLine(both))
        assertEquals("A job is running on a head unit. Update or uninstall once it has finished.", jobLine(both))

        // Up to date: Uninstall alone.
        val upToDate = get("plain", held("1.1.0", has("plain", "1.0.0")), busy = true)
        assertEquals("A job is running on a head unit. Uninstall once it has finished.", jobLine(upToDate))

        // The agent's update: Update alone, as the agent has no Uninstall.
        val agent = get(AGENT_PACKAGE_ID, held("1.0.2"), stored = listOf("1.1.0"), busy = true)
        assertFalse(agent.canInstall)
        assertEquals("A job is running on a head unit. Update once it has finished.", jobLine(agent))

        // With no link the job greys nothing: the link is what is missing.
        assertNull(jobLine(get("plain", held("1.1.0", has("plain", "0.9.0"), live = false), stored = listOf("1.0.0"), busy = true)))
    }

    @Test
    fun `the page knows an update is waiting as the list's row does`() {
        assertTrue(get("plain", held("1.1.0", has("plain", "0.9.0"))).updateAvailable)
        assertFalse(get("plain", held("1.1.0", has("plain", "1.0.0"))).updateAvailable)
        assertFalse(get("plain").updateAvailable)
        assertFalse(get("later", held("1.1.0", has("later", "0.5.0"))).updateAvailable)
    }

    // ---- the download ----

    @Test
    fun `the download is offered only where the head unit is not up to date`() {
        assertTrue(get("plain").showsDownload)
        assertTrue(get("plain", held("1.1.0", has("plain", "0.9.0"))).showsDownload)
        // Up to date: nothing to download or install, whether the archive is on the phone or not.
        val upToDate = get("plain", held("1.1.0", has("plain", "1.0.0")))
        assertEquals(listOf("plain"), upToDate.toDownload.map { it.pkg.id })
        assertFalse(upToDate.showsDownload)
        assertFalse(get("plain", held("1.1.0", has("plain", "1.0.0")), stored = listOf("1.0.0")).showsDownload)
    }

    // ---- the header ----

    @Test
    fun `under the name the page marks the installed version, and an update that waits`() {
        assertEquals(HeaderMarks(), headerMarks(null))
        assertEquals(HeaderMarks(), headerMarks(get("plain")))
        assertEquals(HeaderMarks(installed = "Installed 1.0.0"), headerMarks(get("plain", held("1.1.0", has("plain", "1.0.0")))))
        assertEquals(
            HeaderMarks(installed = "Installed 0.9.0", update = "Update to 1.0.0"),
            headerMarks(get("plain", held("1.1.0", has("plain", "0.9.0")))),
        )
        // From the saved list, with no link: marked all the same.
        assertEquals(
            HeaderMarks(installed = "Installed 0.9.0", update = "Update to 1.0.0"),
            headerMarks(get("plain", held("1.1.0", has("plain", "0.9.0"), live = false))),
        )
        // A version this head unit's agent cannot run is no update.
        assertEquals(HeaderMarks(installed = "Installed 0.5.0"), headerMarks(get("later", held("1.1.0", has("later", "0.5.0")))))
    }

    // ---- About ----

    @Test
    fun `About is laid out at eight lines, cut to five once it is known to be longer, and whole when open`() {
        assertEquals(8, aboutMaxLines(open = false, long = null))
        assertEquals(8, aboutMaxLines(open = false, long = false))
        assertEquals(5, aboutMaxLines(open = false, long = true))
        assertEquals(Int.MAX_VALUE, aboutMaxLines(open = true, long = true))
        assertEquals(Int.MAX_VALUE, aboutMaxLines(open = true, long = null))
    }

    @Test
    fun `a website is an https address with a host, and nothing else`() {
        assertEquals("https://example.com/laptimer", website("https://example.com/laptimer"))
        assertEquals("https://example.com", website("  https://example.com  "))
        assertEquals("HTTPS://example.com", website("HTTPS://example.com"))
        assertNull(website("http://example.com"))
        assertNull(website("ftp://example.com/file"))
        assertNull(website("javascript:alert(1)"))
        assertNull(website("file:///sdcard/x"))
        assertNull(website("https:example.com"))
        assertNull(website("https://"))
        assertNull(website("https:///path"))
        assertNull(website("not an address at all"))
        assertNull(website("https://exa mple.com"))
        assertNull(website(""))
        assertNull(website("   "))
        assertNull(website(null))
    }

    // ---- what is new ----

    private fun entries(count: Int) = (count downTo 1).map { ChangelogRow(ChangelogEntry("1.$it.0", listOf("Change $it")), prerelease = false) }

    @Test
    fun `what is new shows three, and opens to all of up to ten`() {
        val three = entries(3)
        assertEquals(ShownList(three), shownChangelog(three, open = false, website = true))
        assertEquals(ShownList(three), shownChangelog(three, open = true, website = true))

        val five = entries(5)
        assertEquals(ShownList(five.take(3), toggle = "Show all 5 releases"), shownChangelog(five, open = false, website = true))
        assertEquals(ShownList(five, toggle = "Show fewer"), shownChangelog(five, open = true, website = true))

        val ten = entries(10)
        assertEquals(ShownList(ten.take(3), toggle = "Show all 10 releases"), shownChangelog(ten, open = false, website = true))
        assertEquals(ShownList(ten, toggle = "Show fewer"), shownChangelog(ten, open = true, website = true))
    }

    @Test
    fun `what is new opens to the newest ten of more, and says so, pointing to the website where there is one`() {
        val many = entries(14)
        assertEquals(ShownList(many.take(3), toggle = "Show 10 of 14 releases"), shownChangelog(many, open = false, website = true))

        val open = shownChangelog(many, open = true, website = true)
        assertEquals(many.take(10), open.rows)
        assertEquals("1.14.0", open.rows.first().entry.version)
        assertEquals("Show fewer", open.toggle)
        assertEquals("Showing the newest 10 of 14 releases. For the rest, see the package's website.", open.notice)
        assertEquals("Showing the newest 10 of 14 releases.", shownChangelog(many, open = true, website = false).notice)
    }

    @Test
    fun `what is new has each version's release date, where the catalogue has one, and marks the installed one`() {
        val pkg = CataloguePackage(
            id = "plain",
            name = "Plain",
            versions = listOf(version("1.0.0", released = "2026-10-01"), version("0.9.0", released = "2026-09-01"), version("0.8.0")),
            prereleases = listOf(version("1.1.0-rc.1", released = "2026-11-01")),
        )
        val details = PackageDetails(
            id = "plain",
            changelog = listOf(
                ChangelogEntry("1.0.0", listOf("New")),
                ChangelogEntry("0.9.0", listOf("Older")),
                ChangelogEntry("0.8.0", listOf("No date")),
                ChangelogEntry("0.7.0", listOf("Not listed")),
            ),
            prereleaseChangelog = listOf(ChangelogEntry("1.1.0-rc.1", listOf("Try this"))),
        )

        val rows = changelog(details, pkg, installed = "0.9.0")
        assertEquals(
            listOf(
                Triple("1.1.0-rc.1", "2026-11-01", false),
                Triple("1.0.0", "2026-10-01", false),
                Triple("0.9.0", "2026-09-01", true),
                Triple("0.8.0", null, false),
                Triple("0.7.0", null, false),
            ),
            rows.map { Triple(it.entry.version, it.released, it.installed) },
        )
        // Nothing installed, or what is installed has no entry: nothing marked.
        assertTrue(changelog(details, pkg).none { it.installed })
        assertTrue(changelog(details, pkg, installed = "0.6.0").none { it.installed })
        // With no package to look the dates up in, none.
        assertTrue(changelog(details, null, installed = "0.9.0").all { it.released == null })
    }

    // ---- every version ----

    /** A package with [count] stable versions, 1.1.0 to 1.[count].0, listed newest first. */
    private fun many(count: Int) = CataloguePackage(id = "many", name = "Many", versions = (count downTo 1).map { version("1.$it.0") })

    @Test
    fun `every version shows one, the installed where it is listed, and opens to all of up to ten`() {
        val two = allVersions(many(2))
        assertEquals(ShownList(two.take(1), toggle = "Show all 2 versions"), shownVersions(two, open = false, website = true))
        assertEquals(ShownList(two, toggle = "Show fewer"), shownVersions(two, open = true, website = true))

        // Installed, and listed: that is the one row while closed, and it is marked.
        val five = allVersions(many(5), installed = "1.2.0")
        val closed = shownVersions(five, open = false, website = true)
        assertEquals(listOf("1.2.0"), closed.rows.map { it.version.version })
        assertTrue(closed.rows.single().installed)
        assertEquals("Show all 5 versions", closed.toggle)
        assertEquals(ShownList(five, toggle = "Show fewer"), shownVersions(five, open = true, website = true))

        // Ten, the installed the oldest of them: all ten, nothing added.
        val ten = allVersions(many(10), installed = "1.1.0")
        assertEquals("Show all 10 versions", shownVersions(ten, open = false, website = true).toggle)
        assertEquals(ShownList(ten, toggle = "Show fewer"), shownVersions(ten, open = true, website = true))
    }

    @Test
    fun `every version opens to the newest ten of more, and adds the installed one where it is older`() {
        // Installed within the newest ten.
        val inside = allVersions(many(14), installed = "1.8.0")
        val closed = shownVersions(inside, open = false, website = true)
        assertEquals(listOf("1.8.0"), closed.rows.map { it.version.version })
        assertEquals("Show 10 of 14 versions", closed.toggle)
        val open = shownVersions(inside, open = true, website = true)
        assertEquals(inside.take(10), open.rows)
        assertNull(open.extra)
        assertEquals("Show fewer", open.toggle)
        assertEquals("Showing the newest 10 of 14 versions. For the rest, see the package's website.", open.notice)
        assertEquals("Showing the newest 10 of 14 versions.", shownVersions(inside, open = true, website = false).notice)

        // Installed, older than the newest ten: added after them, and the notice says so.
        val outside = allVersions(many(14), installed = "1.2.0")
        assertEquals(listOf("1.2.0"), shownVersions(outside, open = false, website = true).rows.map { it.version.version })
        val added = shownVersions(outside, open = true, website = true)
        assertEquals((14 downTo 5).map { "1.$it.0" }, added.rows.map { it.version.version })
        assertEquals("1.2.0", added.extra?.version?.version)
        assertTrue(added.extra!!.installed)
        assertEquals(
            "Showing the newest 10 of 14 versions, and the installed one. For the rest, see the package's website.",
            added.notice,
        )
        assertEquals("Showing the newest 10 of 14 versions, and the installed one.", shownVersions(outside, open = true, website = false).notice)
    }

    @Test
    fun `an installed version the catalogue does not list, or none, leaves every version as it is`() {
        for (installed in listOf(null, "0.1.0", "not a version")) {
            val rows = allVersions(many(14), installed)
            assertTrue(rows.none { it.installed })
            assertEquals(listOf("1.14.0"), shownVersions(rows, open = false, website = true).rows.map { it.version.version })
            val open = shownVersions(rows, open = true, website = true)
            assertNull(open.extra)
            assertEquals(10, open.rows.size)
            assertEquals("Showing the newest 10 of 14 versions. For the rest, see the package's website.", open.notice)
        }
        // Stable and pre-release versions alike are marked.
        val pre = CataloguePackage(id = "p", name = "P", versions = listOf(version("1.0.0")), prereleases = listOf(version("1.1.0-rc.1")))
        assertEquals(listOf("1.1.0-rc.1" to false, "1.0.0" to true), allVersions(pre, installed = "1.0.0").map { it.version.version to it.installed })
        assertEquals(listOf("1.1.0-rc.1" to true, "1.0.0" to false), allVersions(pre, installed = "1.1.0-rc.1").map { it.version.version to it.installed })
    }
}

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
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.RestartKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What a package's page ([cataloguePackageUiState]) offers of uninstalling it, and what it greys while a job that
 * needs the phone is running.
 */
class PackagePageTest {

    private fun version(version: String, agent: String? = ">=1.0.0") = PackageVersion(
        version = version,
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
}

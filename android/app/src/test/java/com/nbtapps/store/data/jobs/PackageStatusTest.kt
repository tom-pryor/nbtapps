// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.FreeSpace
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.JobResult
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.StepResult
import com.nbtapps.store.model.TcpEndpoint
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** What the head unit holds ([holdings]) and where a catalogue package stands on it ([packageStatus], [agentUpdate]). */
class PackageStatusTest {

    private fun version(version: String, agent: String? = null) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = "00",
        manifestSha256 = "11",
        requires = PackageRequirements(agent),
    )

    /** Versions newest first, as the catalogue parser leaves them. */
    private fun pkg(id: String, vararg versions: PackageVersion, name: String = id) =
        CataloguePackage(id = id, name = name, versions = versions.toList())

    private fun has(id: String, version: String) = InstalledPackage(id, id, version)

    private val tcp = TcpEndpoint("192.168.0.1", 7144)

    private fun profile(agent: String, installed: List<InstalledPackage>, at: Long, restart: String = RestartKind.NONE) =
        HeadUnitProfile(agent, 1, FreeSpace(1, 1, 1, 1), restart, installed, seenAt = at)

    private fun connected(id: String, agent: String, installed: List<InstalledPackage>, restart: String = RestartKind.NONE) =
        ConnectionState.Connected(id, tcp, AgentInfo(agent, "000102030405060708090a0b0c0d0e0f", 1, restartNeeded = restart), installed)

    // ---- holdings ----

    @Test
    fun `with no head unit saved, or one never spoken to, nothing is held`() {
        assertNull(holdings(emptyList(), ConnectionState.Disconnected))
        assertNull(holdings(listOf(HeadUnit("car", "Car", listOf(tcp))), ConnectionState.Disconnected))
        // A link that is being opened to it has said nothing yet either.
        assertNull(holdings(listOf(HeadUnit("car", "Car", listOf(tcp))), ConnectionState.Connecting("car")))
    }

    @Test
    fun `with no link, the saved profile is what the head unit holds, and is not live`() {
        val car = HeadUnit("car", "Car", listOf(tcp), profile = profile("1.0.2", listOf(has("lib", "2.0.0")), at = 77, restart = RestartKind.HMI))

        val held = requireNotNull(holdings(listOf(car), ConnectionState.Disconnected))

        assertEquals("car", held.headUnitId)
        assertEquals("Car", held.name)
        assertFalse(held.live)
        assertEquals("1.0.2", held.agentVersion)
        assertEquals(listOf(has("lib", "2.0.0")), held.installed)
        assertEquals(77L, held.seenAt)
        // The agent's mark does not outlast a power down, so a saved one says nothing of now.
        assertEquals(RestartKind.NONE, held.restartNeeded)
        // A link that failed, or is being opened, changes nothing of that.
        assertEquals(held, holdings(listOf(car), ConnectionState.Failed("car", "No route")))
        assertEquals(held, holdings(listOf(car), ConnectionState.Connecting("car")))
    }

    @Test
    fun `over a link, what the agent says now is what the head unit holds`() {
        val car = HeadUnit("car", "Car", listOf(tcp), profile = profile("1.0.0", emptyList(), at = 77))
        val now = connected("car", "1.0.2", listOf(has("lib", "2.1.0")), restart = RestartKind.REBOOT)

        val held = requireNotNull(holdings(listOf(car), now))

        assertTrue(held.live)
        assertEquals("1.0.2", held.agentVersion)
        assertEquals(listOf(has("lib", "2.1.0")), held.installed)
        assertEquals(RestartKind.REBOOT, held.restartNeeded)
    }

    @Test
    fun `the head unit is the one the app is working with, as the top bar has it`() {
        val car = HeadUnit("car", "Car", listOf(tcp), profile = profile("1.0.0", listOf(has("lib", "1.0.0")), at = 5))
        val van = HeadUnit("van", "Van", listOf(tcp), profile = profile("1.0.2", listOf(has("lib", "2.0.0")), at = 9))

        // With no link, the one spoken to last.
        assertEquals("van", holdings(listOf(car, van), ConnectionState.Disconnected)?.headUnitId)
        // A link to the other makes it the one, with what it says.
        val live = requireNotNull(holdings(listOf(car, van), connected("car", "1.0.0", emptyList())))
        assertEquals("car", live.headUnitId)
        assertTrue(live.live)
        // A link to one that is no longer saved is none of the saved ones'.
        val other = requireNotNull(holdings(listOf(car, van), connected("gone", "9.9.9", emptyList())))
        assertEquals("van", other.headUnitId)
        assertFalse(other.live)
    }

    // ---- packageStatus ----

    @Test
    fun `with no head unit known, the newest version is on offer and nothing is installed`() {
        val status = requireNotNull(packageStatus(pkg("ui", version("0.3.0", ">=1.1.0"), version("0.2.0")), emptyList(), agentVersion = null))

        assertEquals("0.3.0", status.offered.version)
        assertTrue(status.suits)
        assertNull(status.installed)
        assertNull(status.heldBack)
        assertFalse(status.updateAvailable)
        assertFalse(status.upToDate)
    }

    @Test
    fun `a package with no version has no status`() {
        assertNull(packageStatus(pkg("empty"), emptyList(), "1.0.0"))
    }

    @Test
    fun `an older installed version than the one on offer is an update`() {
        val status = requireNotNull(packageStatus(pkg("lib", version("2.1.0"), version("2.0.0")), listOf(has("lib", "2.0.0")), "1.0.0"))

        assertEquals("2.0.0", status.installed)
        assertEquals("2.1.0", status.offered.version)
        assertTrue(status.updateAvailable)
        assertFalse(status.upToDate)
    }

    @Test
    fun `the offered version or a newer one on the head unit is up to date`() {
        val lib = pkg("lib", version("2.1.0"), version("2.0.0"))

        val same = requireNotNull(packageStatus(lib, listOf(has("lib", "2.1.0")), "1.0.0"))
        assertTrue(same.upToDate)
        assertFalse(same.updateAvailable)

        val newer = requireNotNull(packageStatus(lib, listOf(has("lib", "3.0.0")), "1.0.0"))
        assertTrue(newer.upToDate)
        assertFalse(newer.updateAvailable)
    }

    @Test
    fun `a newest version the agent is too old for is held back, and the newest that suits is on offer`() {
        val ui = pkg("ui", version("0.3.0", ">=1.1.0"), version("0.2.1", ">=1.0.0"), version("0.2.0", ">=1.0.0"))

        val current = requireNotNull(packageStatus(ui, listOf(has("ui", "0.2.1")), "1.0.2"))
        assertEquals("0.2.1", current.offered.version)
        assertTrue(current.suits)
        assertEquals("0.3.0", current.heldBack?.version)
        assertTrue(current.upToDate)
        assertFalse(current.updateAvailable)

        // An update to the newest that suits is still one.
        val behind = requireNotNull(packageStatus(ui, listOf(has("ui", "0.2.0")), "1.0.2"))
        assertTrue(behind.updateAvailable)
        assertEquals("0.2.1", behind.offered.version)
        assertEquals("0.3.0", behind.heldBack?.version)

        // An agent that runs the newest holds nothing back.
        val free = requireNotNull(packageStatus(ui, listOf(has("ui", "0.2.1")), "1.1.0"))
        assertEquals("0.3.0", free.offered.version)
        assertNull(free.heldBack)
        assertTrue(free.updateAvailable)
    }

    @Test
    fun `a package no version of which suits the agent offers its newest, and no update`() {
        val ui = pkg("ui", version("0.3.0", ">=2.0.0"), version("0.2.0", ">=1.5.0"))

        val absent = requireNotNull(packageStatus(ui, emptyList(), "1.0.2"))
        assertEquals("0.3.0", absent.offered.version)
        assertFalse(absent.suits)
        assertNull(absent.heldBack)
        assertFalse(absent.updateAvailable)

        // Installed, as by an agent that was newer once: there is still nothing this one can update it to.
        val there = requireNotNull(packageStatus(ui, listOf(has("ui", "0.1.0")), "1.0.2"))
        assertFalse(there.updateAvailable)
    }

    @Test
    fun `the agent's own package counts as installed in the version that runs, whatever the list says`() {
        val agent = pkg(AGENT_PACKAGE_ID, version("1.1.0", ">=1.0.0"), version("1.0.2", ">=1.0.0"))

        // The first install is a script's and leaves no record.
        val unlisted = requireNotNull(packageStatus(agent, emptyList(), "1.0.2"))
        assertEquals("1.0.2", unlisted.installed)
        assertTrue(unlisted.updateAvailable)

        // A record of an earlier update does not speak for the agent that runs now.
        val listed = requireNotNull(packageStatus(agent, listOf(has(AGENT_PACKAGE_ID, "1.0.0")), "1.1.0"))
        assertEquals("1.1.0", listed.installed)
        assertTrue(listed.upToDate)

        // With no head unit known it is a package like any other.
        assertNull(requireNotNull(packageStatus(agent, emptyList(), agentVersion = null)).installed)
        assertEquals("1.0.2", installedVersion(AGENT_PACKAGE_ID, emptyList(), "1.0.2"))
        assertNull(installedVersion("lib", emptyList(), "1.0.2"))
    }

    // ---- agentUpdate ----

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test"),
        serial = 1,
        packages = listOf(
            pkg(AGENT_PACKAGE_ID, version("1.1.0", ">=1.0.0"), name = "NBTApp agent"),
            pkg("ui", version("0.3.0", ">=1.1.0"), version("0.2.1", ">=1.0.0"), name = "App Launcher"),
            pkg("logs", version("0.9.0", ">=1.1.0"), version("0.8.0", ">=1.0.0"), name = "Log Viewer"),
            pkg("far", version("5.0.0", ">=2.0.0"), version("4.0.0", ">=1.0.0"), name = "Far Off"),
            pkg("lib", version("2.1.0"), name = "Runtime"),
        ),
    )

    private fun held(agent: String, vararg installed: InstalledPackage) =
        Holdings("car", "Car", live = true, agentVersion = agent, installed = installed.toList(), restartNeeded = RestartKind.NONE, seenAt = 1)

    @Test
    fun `a newer agent in the catalogue is an update, with the versions that wait for it, installed packages first`() {
        val update = requireNotNull(agentUpdate(index, held("1.0.2", has("logs", "0.8.0"))))

        assertEquals("1.1.0", update.version)
        assertEquals("1.0.2", update.running)
        // Not Far Off 5.0.0: the newer agent is too old for it as well.
        assertEquals(listOf("Log Viewer 0.9.0", "App Launcher 0.3.0"), update.unblocks.map { it.label })
        assertEquals(listOf("0.8.0", null), update.unblocks.map { it.installed })
    }

    // ---- unblockedBy ----

    @Test
    fun `a newer agent brings the newest version it runs, where the older one ran none or an older one`() {
        val catalogue = index.copy(
            packages = index.packages + listOf(
                // The older agent runs no version of it at all: it could not be installed.
                pkg("plugin", version("1.0.0", ">=1.1.0"), name = "iDrive plugin"),
                // The newest is beyond the newer agent too, and the one below it is what that agent brings.
                pkg("steps", version("3.0.0", ">=2.0.0"), version("2.0.0", ">=1.1.0"), version("1.0.0", ">=1.0.0"), name = "Steps"),
                // Only for agents below the newer one: nothing is brought.
                pkg("old", version("2.0.0", ">=1.0.0 <1.1.0"), version("1.0.0"), name = "Old"),
            ),
        )

        val brought = unblockedBy(catalogue, listOf(has("steps", "1.0.0")), older = "1.0.2", newer = "1.1.0")

        assertEquals(listOf("Steps 2.0.0", "App Launcher 0.3.0", "iDrive plugin 1.0.0", "Log Viewer 0.9.0"), brought.map { it.label })
        assertEquals(listOf("steps", "ui", "plugin", "logs"), brought.map { it.id })
        assertEquals("1.0.0", brought.first().installed)
    }

    @Test
    fun `what the head unit already has, and the agent's own package, are not brought`() {
        // Put there before the agent was too old for it, or by a newer agent that has since gone back.
        assertEquals(listOf("logs"), unblockedBy(index, listOf(has("ui", "0.3.0")), "1.0.2", "1.1.0").map { it.id })
        assertEquals(emptyList<Unblocked>(), unblockedBy(index, listOf(has("ui", "0.4.0"), has("logs", "0.9.0")), "1.0.2", "1.1.0"))
        // The same agent, and one that went down a version, bring nothing.
        assertEquals(emptyList<Unblocked>(), unblockedBy(index, emptyList(), "1.1.0", "1.1.0"))
        assertEquals(emptyList<Unblocked>(), unblockedBy(index, emptyList(), "1.1.0", "1.0.2"))
    }

    private fun result(state: String, vararg steps: StepResult) = JobResult("1011121314151617", state, steps.toList())

    @Test
    fun `a job that updated the agent and went well brings what the new version runs`() {
        val update = StepResult("upgrade", AGENT_PACKAGE_ID, "1.1.0", oldVersion = "1.0.2", result = "done")

        assertEquals(listOf("ui", "logs"), unblockedBy(index, emptyList(), result(JobState.SUCCEEDED, update)).map { it.id })
        // With other steps beside it, as a job has that installs something along with the agent it needs.
        val other = StepResult("install", "lib", "2.1.0", result = "done")
        assertEquals(listOf("ui", "logs"), unblockedBy(index, emptyList(), result(JobState.SUCCEEDED, other, update)).map { it.id })
    }

    @Test
    fun `any other job brings nothing`() {
        val update = StepResult("upgrade", AGENT_PACKAGE_ID, "1.1.0", oldVersion = "1.0.2", result = "done")

        // The new version did not start, and the one before is back.
        assertEquals(emptyList<Unblocked>(), unblockedBy(index, emptyList(), result(JobState.FAILED, update.copy(result = "failed"))))
        assertEquals(emptyList<Unblocked>(), unblockedBy(index, emptyList(), result(JobState.FAILED, update)))
        // Not the agent's package, and an agent step that names no version it came from.
        assertEquals(emptyList<Unblocked>(), unblockedBy(index, emptyList(), result(JobState.SUCCEEDED, StepResult("upgrade", "ui", "0.2.1", "0.2.0", "done"))))
        assertEquals(emptyList<Unblocked>(), unblockedBy(index, emptyList(), result(JobState.SUCCEEDED, update.copy(oldVersion = null))))
        assertEquals(emptyList<Unblocked>(), unblockedBy(index, emptyList(), result(JobState.SUCCEEDED)))
    }

    @Test
    fun `there is no agent update for an agent that is current, a catalogue without one, or no head unit`() {
        assertNull(agentUpdate(index, held("1.1.0")))
        assertNull(agentUpdate(index, held("2.0.0")))
        assertNull(agentUpdate(index.copy(packages = index.packages.drop(1)), held("1.0.2")))
        assertNull(agentUpdate(index, null))
    }
}

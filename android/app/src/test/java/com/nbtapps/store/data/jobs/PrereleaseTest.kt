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
import com.nbtapps.store.model.RestartKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Which versions a package offers by its pre-release switch ([offeredVersions]), and what follows from that for
 * its status, the agent's update, what a newer agent brings and the resolver.
 */
class PrereleaseTest {

    private fun version(version: String, agent: String? = null, vararg depends: Pair<String, String?>) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = "00",
        manifestSha256 = "11",
        requires = PackageRequirements(agent),
        depends = depends.map { PackageDependency(it.first, it.second) },
    )

    /** Each list newest first, as the catalogue parser leaves them. */
    private fun pkg(id: String, stable: List<PackageVersion>, prereleases: List<PackageVersion> = emptyList(), name: String = id) =
        CataloguePackage(id = id, name = name, versions = stable, prereleases = prereleases)

    private fun has(id: String, version: String) = InstalledPackage(id, id, version)

    private val on = setOf("lib")

    private fun status(pkg: CataloguePackage, installed: String?, switchedOn: Set<String>, agent: String = "1.0.0") =
        requireNotNull(packageStatus(pkg, listOfNotNull(installed?.let { has(pkg.id, it) }), agent, switchedOn))

    // ---- offeredVersions ----

    @Test
    fun `with the switch off only stable versions count, and with it on the newest of either comes first`() {
        val lib = pkg("lib", listOf(version("1.1.0"), version("1.0.0")), listOf(version("1.2.0-rc.1"), version("1.1.0-rc.1")))

        assertEquals(listOf("1.1.0", "1.0.0"), offeredVersions(lib, false).map { it.version })
        assertEquals(listOf("1.2.0-rc.1", "1.1.0", "1.1.0-rc.1", "1.0.0"), offeredVersions(lib, true).map { it.version })
        // By the set of switches, each package by its own.
        assertEquals(offeredVersions(lib, true), offeredVersions(lib, on))
        assertEquals(offeredVersions(lib, false), offeredVersions(lib, setOf("other")))
        // A package without pre-releases is the same either way.
        val plain = pkg("plain", listOf(version("1.0.0")))
        assertEquals(offeredVersions(plain, false), offeredVersions(plain, true))
    }

    // ---- packageStatus ----

    @Test
    fun `with the switch off a newer pre-release is not offered`() {
        val lib = pkg("lib", listOf(version("1.0.0")), listOf(version("1.1.0-rc.1")))

        val status = status(lib, "1.0.0", emptySet())
        assertEquals("1.0.0", status.offered.version)
        assertFalse(status.offeredPrerelease)
        assertFalse(status.installedPrerelease)
        assertTrue(status.upToDate)
        assertFalse(status.updateAvailable)
        assertFalse(status.needsSwitch)
    }

    @Test
    fun `with the switch on the newest of either is offered, and a pre-release is marked one`() {
        val lib = pkg("lib", listOf(version("1.0.0")), listOf(version("1.1.0-rc.1")))

        val status = status(lib, "1.0.0", on)
        assertEquals("1.1.0-rc.1", status.offered.version)
        assertTrue(status.offeredPrerelease)
        assertFalse(status.installedPrerelease)
        assertTrue(status.updateAvailable)

        // A stable version above the pre-release is the newest, and is offered.
        val newer = status(pkg("lib", listOf(version("1.1.0"), version("1.0.0")), listOf(version("1.1.0-rc.1"))), "1.0.0", on)
        assertEquals("1.1.0", newer.offered.version)
        assertFalse(newer.offeredPrerelease)
    }

    @Test
    fun `with the switch on, a pre-release the agent is too old for is held back like any other version`() {
        val lib = pkg("lib", listOf(version("1.0.0", ">=1.0.0")), listOf(version("1.1.0-rc.1", ">=1.5.0")))

        val status = status(lib, "1.0.0", on, agent = "1.0.0")
        assertEquals("1.0.0", status.offered.version)
        assertEquals("1.1.0-rc.1", status.heldBack?.version)
        assertTrue(status.upToDate)

        val newer = status(lib, "1.0.0", on, agent = "1.5.0")
        assertEquals("1.1.0-rc.1", newer.offered.version)
        assertNull(newer.heldBack)
    }

    @Test
    fun `with the switch off, a pre-release on the head unit is up to date until a stable version above it is out`() {
        val lib = pkg("lib", listOf(version("1.0.0")), listOf(version("1.1.0-rc.2"), version("1.1.0-rc.1")))

        val status = status(lib, "1.1.0-rc.1", emptySet())
        assertTrue(status.installedPrerelease)
        assertEquals("1.0.0", status.offered.version)
        assertTrue(status.upToDate)
        // Not to rc.2: newer pre-releases are not offered with the switch off.
        assertFalse(status.updateAvailable)

        // With the switch on it is.
        val switched = status(lib, "1.1.0-rc.1", on)
        assertEquals("1.1.0-rc.2", switched.offered.version)
        assertTrue(switched.updateAvailable)
    }

    @Test
    fun `a stable version supersedes a pre-release, the version it led up to`() {
        val tagged = pkg("lib", listOf(version("1.1.0"), version("1.0.0")), listOf(version("1.1.0-rc.1")))

        val status = status(tagged, "1.1.0-rc.1", emptySet())
        assertTrue(status.installedPrerelease)
        assertEquals("1.1.0", status.offered.version)
        assertFalse(status.offeredPrerelease)
        assertTrue(status.updateAvailable)
    }

    @Test
    fun `a stable version supersedes a pre-release, one past it with the version it led up to skipped`() {
        val skipped = pkg("lib", listOf(version("1.2.0"), version("1.0.0")), listOf(version("1.1.0-rc.1")))

        val status = status(skipped, "1.1.0-rc.1", emptySet())
        assertEquals("1.2.0", status.offered.version)
        assertTrue(status.updateAvailable)
    }

    @Test
    fun `a stable version supersedes a pre-release, the same version promoted`() {
        // Published as a pre-release with a plain version, then moved to the stable list as it was.
        val before = pkg("lib", listOf(version("1.0.0")), listOf(version("1.1.0")))
        val after = pkg("lib", listOf(version("1.1.0"), version("1.0.0")))

        val was = status(before, "1.1.0", emptySet())
        assertTrue(was.installedPrerelease)
        assertTrue(was.upToDate)

        val now = status(after, "1.1.0", emptySet())
        assertFalse(now.installedPrerelease)
        assertFalse(now.offeredPrerelease)
        assertTrue(now.upToDate)
        assertFalse(now.updateAvailable)

        // One promoted with its pre-release part stays a pre-release by its version, and is up to date.
        val rc = status(pkg("lib", listOf(version("1.1.0-rc.1"), version("1.0.0"))), "1.1.0-rc.1", emptySet())
        assertTrue(rc.installedPrerelease)
        assertTrue(rc.upToDate)
    }

    @Test
    fun `a package out only in pre-release versions is listed, and waits for its switch`() {
        val early = pkg("early", emptyList(), listOf(version("0.2.0-beta.1"), version("0.1.0-beta.1")))

        val absent = requireNotNull(packageStatus(early, emptyList(), "1.0.0"))
        assertTrue(absent.needsSwitch)
        assertEquals("0.2.0-beta.1", absent.offered.version)
        assertTrue(absent.offeredPrerelease)
        assertFalse(absent.updateAvailable)
        assertFalse(absent.upToDate)

        // On the head unit already, from before the switch was turned off: up to date, and nothing newer offered.
        val there = requireNotNull(packageStatus(early, listOf(has("early", "0.1.0-beta.1")), "1.0.0"))
        assertTrue(there.upToDate)
        assertFalse(there.updateAvailable)
        assertTrue(there.installedPrerelease)

        // Switched on, it is a package like any other.
        val switched = requireNotNull(packageStatus(early, listOf(has("early", "0.1.0-beta.1")), "1.0.0", setOf("early")))
        assertFalse(switched.needsSwitch)
        assertTrue(switched.updateAvailable)

        // With no head unit known it is listed too.
        assertTrue(requireNotNull(packageStatus(early, emptyList(), null)).needsSwitch)
    }

    // ---- agentUpdate and unblockedBy ----

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test"),
        serial = 1,
        packages = listOf(
            pkg(AGENT_PACKAGE_ID, listOf(version("1.1.0", ">=1.0.0")), listOf(version("1.2.0-rc.1", ">=1.0.0")), name = "NBTApp agent"),
            pkg("ui", listOf(version("0.2.1", ">=1.0.0")), listOf(version("0.3.0-rc.1", ">=1.2.0-rc.1")), name = "App Launcher"),
            pkg("logs", listOf(version("0.9.0", ">=1.1.0"), version("0.8.0", ">=1.0.0")), listOf(version("0.10.0-rc.1", ">=1.1.0")), name = "Log Viewer"),
            pkg("lib", listOf(version("2.0.0"), version("1.0.0")), listOf(version("2.1.0-rc.1")), name = "Runtime"),
            pkg("radio", listOf(version("1.0.0", null, "lib" to ">=2.0.0 <3.0.0")), listOf(version("1.1.0-rc.1", null, "lib" to ">=2.1.0-rc.1")), name = "Radio"),
            pkg("early", emptyList(), listOf(version("0.1.0-beta.1", ">=1.0.0")), name = "Early Bird"),
            pkg("needsearly", listOf(version("1.0.0", null, "early" to null)), name = "Needs Early"),
        ),
    )

    private fun held(agent: String, vararg installed: InstalledPackage) =
        Holdings("car", "Car", live = true, agentVersion = agent, installed = installed.toList(), restartNeeded = RestartKind.NONE, seenAt = 1)

    @Test
    fun `the agent's update is a pre-release only with the agent's switch on, and says so`() {
        val stable = requireNotNull(agentUpdate(index, held("1.0.2")))
        assertEquals("1.1.0", stable.version)
        assertFalse(stable.prerelease)

        val switched = requireNotNull(agentUpdate(index, held("1.0.2"), setOf(AGENT_PACKAGE_ID)))
        assertEquals("1.2.0-rc.1", switched.version)
        assertTrue(switched.prerelease)

        // On a pre-release agent with the switch turned off: nothing, until a stable agent above it is out.
        assertNull(agentUpdate(index, held("1.2.0-rc.1")))
    }

    @Test
    fun `what a newer agent brings is judged by each package's own switch`() {
        // Every switch off: Log Viewer's stable 0.9.0, and not App Launcher, whose only newer version is a pre-release.
        assertEquals(listOf("Log Viewer 0.9.0"), unblockedBy(index, emptyList(), "1.0.2", "1.2.0-rc.1").map { it.label })
        // Their switches on: the newest each has that the newer agent runs.
        assertEquals(
            listOf("App Launcher 0.3.0-rc.1", "Log Viewer 0.10.0-rc.1"),
            unblockedBy(index, emptyList(), "1.0.2", "1.2.0-rc.1", setOf("ui", "logs")).map { it.label },
        )
        // The agent's switch on, the others off: the card offers the pre-release agent and only what is stable.
        val update = requireNotNull(agentUpdate(index, held("1.0.2"), setOf(AGENT_PACKAGE_ID)))
        assertEquals(listOf("Log Viewer 0.9.0"), update.unblocks.map { it.label })
    }

    // ---- the agent a version needs ----

    @Test
    fun `a version that only a pre-release agent runs asks for the agent's switch`() {
        val launcher = index.packages.first { it.id == "ui" }.prereleases.single()

        assertEquals("1.2.0-rc.1", agentPrereleaseFor(index, launcher, "1.0.2", emptySet())?.version)
        // Not with the switch on: the agent's update is then offered as any other.
        assertNull(agentPrereleaseFor(index, launcher, "1.0.2", setOf(AGENT_PACKAGE_ID)))
        // Not where the running agent runs it, and not with no head unit known.
        assertNull(agentPrereleaseFor(index, launcher, "1.2.0-rc.1", emptySet()))
        assertNull(agentPrereleaseFor(index, launcher, null, emptySet()))
        // Not where a stable agent runs it: that is an agent update like any other.
        val logs = index.packages.first { it.id == "logs" }.latest!!
        assertNull(agentPrereleaseFor(index, logs, "1.0.2", emptySet()))
        // Not where no agent in the catalogue runs it.
        assertNull(agentPrereleaseFor(index, version("9.0.0", ">=5.0.0"), "1.0.2", emptySet()))
        // Not where the pre-release agent cannot go on the running one.
        val far = index.copy(packages = index.packages.map { if (it.id == AGENT_PACKAGE_ID) it.copy(prereleases = listOf(version("1.2.0-rc.1", ">=1.1.0"))) else it })
        assertNull(agentPrereleaseFor(far, launcher, "1.0.2", emptySet()))
    }

    // ---- the resolver ----

    private fun resolve(id: String, version: String, switchedOn: Set<String> = emptySet(), installed: List<InstalledPackage> = emptyList()): Resolver.Resolution {
        val pkg = index.packages.first { it.id == id }
        val target = (pkg.versions + pkg.prereleases).first { it.version == version }
        return Resolver.resolve(index, Resolver.Needed(pkg, target), installed, agentVersion = null, switchedOn = switchedOn)
    }

    private fun Resolver.Resolution.names() = needed.map { "${it.pkg.id} ${it.version.version}" }

    @Test
    fun `a dependency takes a pre-release only where its own switch is on`() {
        // Off: lib's stable 2.0.0, though its 2.1.0-rc.1 is in range too.
        assertEquals(listOf("lib 2.0.0", "radio 1.0.0"), resolve("radio", "1.0.0").names())
        // On for lib: the newest in range of either.
        assertEquals(listOf("lib 2.1.0-rc.1", "radio 1.0.0"), resolve("radio", "1.0.0", setOf("lib")).names())
        // On for radio only: what radio needs is judged by lib's switch, not radio's.
        assertEquals(listOf("lib 2.0.0", "radio 1.0.0"), resolve("radio", "1.0.0", setOf("radio")).names())
    }

    @Test
    fun `a dependency that only a pre-release meets is reported apart, so that the user can be asked`() {
        val resolution = resolve("radio", "1.1.0-rc.1", setOf("radio"))

        assertEquals(listOf("radio 1.1.0-rc.1"), resolution.names())
        assertTrue(resolution.unavailable.isEmpty())
        assertEquals(listOf(Resolver.PrereleaseNeed("lib", "2.1.0-rc.1", "radio")), resolution.needsPrerelease)

        // With lib switched on it is taken.
        val switched = resolve("radio", "1.1.0-rc.1", setOf("radio", "lib"))
        assertEquals(listOf("lib 2.1.0-rc.1", "radio 1.1.0-rc.1"), switched.names())
        assertTrue(switched.needsPrerelease.isEmpty())

        // A package out only in pre-releases is asked for the same way.
        val early = resolve("needsearly", "1.0.0")
        assertEquals(listOf(Resolver.PrereleaseNeed("early", "0.1.0-beta.1", "needsearly")), early.needsPrerelease)
        assertTrue(early.unavailable.isEmpty())
    }

    @Test
    fun `a dependency that no pre-release meets either is unavailable, and nothing is asked`() {
        val strict = index.copy(packages = index.packages + pkg("strict", listOf(version("1.0.0", null, "lib" to ">=4.0.0"))))
        val pkg = strict.packages.last()

        val resolution = Resolver.resolve(strict, Resolver.Needed(pkg, pkg.latest!!), emptyList(), null)
        assertEquals(listOf("lib"), resolution.unavailable.map { it.id })
        assertTrue(resolution.needsPrerelease.isEmpty())

        // A pre-release below what is installed is no answer either: the agent refuses a downgrade.
        val below = resolve("radio", "1.0.0", installed = listOf(has("lib", "3.5.0")))
        assertEquals(listOf("lib"), below.unavailable.map { it.id })
        assertTrue(below.needsPrerelease.isEmpty())
    }

    // ---- every switch off ----

    @Test
    fun `with every switch off, an index with pre-releases gives what it gives without them`() {
        val without = index.copy(packages = index.packages.map { it.copy(prereleases = emptyList()) })
        val heads = listOf(
            null,
            held("1.0.2"),
            held("1.1.0", has("logs", "0.8.0"), has("lib", "1.0.0")),
            held("1.2.0", has("ui", "0.2.1"), has("radio", "1.0.0"), has("lib", "2.0.0")),
        )

        for (head in heads) {
            for ((pkg, plain) in index.packages.zip(without.packages)) {
                // The two that an app without channels never saw: what is out only in pre-releases, and what needs it.
                if (pkg.prereleaseOnly || pkg.id == "needsearly") continue
                assertEquals("${pkg.id} on $head", packageStatus(plain, head), packageStatus(pkg, head))
                val target = Resolver.Needed(pkg, pkg.latest!!)
                val with = Resolver.resolve(index, target, head?.installed.orEmpty(), head?.agentVersion)
                val before = Resolver.resolve(without, Resolver.Needed(plain, plain.latest!!), head?.installed.orEmpty(), head?.agentVersion)
                assertEquals(before.names(), with.names())
                assertEquals(before.unavailable, with.unavailable)
            }
            assertEquals("$head", agentUpdate(without, head), agentUpdate(index, head))
            if (head != null) assertEquals(unblockedBy(without, head.installed, "1.0.0", "1.2.0"), unblockedBy(index, head.installed, "1.0.0", "1.2.0"))
        }
        // The one with no stable version is listed, and is never an update; what needs it asks for it, where an
        // app without channels found it missing.
        val early = requireNotNull(packageStatus(index.packages.first { it.id == "early" }, held("1.2.0")))
        assertTrue(early.needsSwitch)
        assertFalse(early.updateAvailable)
        val needs = resolve("needsearly", "1.0.0")
        assertEquals(listOf("needsearly 1.0.0"), needs.names())
        assertEquals(listOf("early"), needs.needsPrerelease.map { it.id })
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.JobResult
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.SemVer

/**
 * The package the agent updates itself with. Its first install is a script's and leaves no record, so the agent
 * does not list it among the installed packages: the version that counts as installed is the one that runs,
 * which is how the agent itself judges an update of it.
 */
const val AGENT_PACKAGE_ID = "org.nbtapp.agent"

/**
 * The versions of [pkg] that are on offer, newest first: its stable versions, and with [prerelease] on (the
 * package's switch, as [PrereleaseRepository][com.nbtapps.store.data.PrereleaseRepository] keeps it) its
 * pre-release versions among them, so that the newest of either is offered. This is the one place that says
 * which versions count: the list, the package's page, the agent's update card and the resolver all take them
 * from here, each package by its own switch. A pre-release is never taken for a package whose switch is off.
 */
fun offeredVersions(pkg: CataloguePackage, prerelease: Boolean): List<PackageVersion> =
    if (!prerelease || pkg.prereleases.isEmpty()) {
        pkg.versions
    } else {
        (pkg.versions + pkg.prereleases).sortedByDescending { SemVer.parseOrNull(it.version) }
    }

/** [offeredVersions] of [pkg], by its switch among the ids [switchedOn]. */
fun offeredVersions(pkg: CataloguePackage, switchedOn: Set<String>): List<PackageVersion> = offeredVersions(pkg, pkg.id in switchedOn)

/**
 * Where a catalogue package stands against a head unit: the one rule the catalogue's list, a package's page and
 * the agent's update card all show, so that they agree. The agent has the last word when a job is planned.
 */
data class PackageStatus(
    /**
     * The version on offer, of the package's [offeredVersions]: the newest that runs on the head unit's agent.
     * With none that does, or with no head unit known, the newest of all.
     */
    val offered: PackageVersion,
    /** Whether [offered] runs on the head unit's agent. True when no head unit is known. */
    val suits: Boolean,
    /** The version on the head unit. Null when the package is not there, and when no head unit is known. */
    val installed: String?,
    /**
     * The catalogue's newest version, when the head unit's agent cannot run it and can run [offered], which is
     * older: what a newer agent would bring.
     */
    val heldBack: PackageVersion?,
    /** [offered] is a pre-release: see [CataloguePackage.isPrerelease]. */
    val offeredPrerelease: Boolean = false,
    /** [installed] is a pre-release: see [CataloguePackage.isPrerelease]. */
    val installedPrerelease: Boolean = false,
    /**
     * The package is out only in pre-release versions, and its switch is off: [offered] is from its pre-release
     * versions, which nothing is offered from until the user turns the switch on. Nothing is an update meanwhile,
     * and a pre-release already on the head unit counts as up to date.
     */
    val needsSwitch: Boolean = false,
) {
    /**
     * [offered] is what the head unit already has, or older than it. With the switch off, a pre-release on the
     * head unit is up to date until a stable version above it is out: newer pre-releases are not offered.
     */
    val upToDate: Boolean
        get() {
            val have = installed?.let(SemVer::parseOrNull) ?: return false
            if (needsSwitch) return true
            val offer = SemVer.parseOrNull(offered.version) ?: return false
            return have >= offer
        }

    /** The head unit has an older version than [offered], and its agent runs [offered]. */
    val updateAvailable: Boolean get() = installed != null && suits && !upToDate
}

/** The version of [id] that counts as installed: for the agent's own package, the agent that runs. */
fun installedVersion(id: String, installed: List<InstalledPackage>, agentVersion: String?): String? =
    if (id == AGENT_PACKAGE_ID && agentVersion != null) agentVersion else installed.find { it.id == id }?.version

/**
 * The status of [pkg] on a head unit that has [installed] and runs an agent of [agentVersion]; with no head unit
 * known, an empty list and null. [switchedOn] are the packages whose pre-release versions are offered. Null for a
 * package that has no version.
 *
 * A package out only in pre-release versions is shown all the same, with the newest of those, so that it can be
 * found: see [PackageStatus.needsSwitch].
 */
fun packageStatus(
    pkg: CataloguePackage,
    installed: List<InstalledPackage>,
    agentVersion: String?,
    switchedOn: Set<String> = emptySet(),
): PackageStatus? {
    val on = pkg.id in switchedOn
    val needsSwitch = !on && pkg.prereleaseOnly
    val versions = offeredVersions(pkg, on || needsSwitch)
    val newest = versions.firstOrNull() ?: return null
    val suitable = versions.firstOrNull { Resolver.suits(it, agentVersion) }
    val offered = suitable ?: newest
    val have = installedVersion(pkg.id, installed, agentVersion)
    return PackageStatus(
        offered = offered,
        suits = suitable != null,
        installed = have,
        heldBack = newest.takeIf { suitable != null && suitable !== newest },
        offeredPrerelease = pkg.isPrerelease(offered.version),
        installedPrerelease = have != null && pkg.isPrerelease(have),
        needsSwitch = needsSwitch,
    )
}

fun packageStatus(pkg: CataloguePackage, holdings: Holdings?, switchedOn: Set<String> = emptySet()): PackageStatus? =
    packageStatus(pkg, holdings?.installed.orEmpty(), holdings?.agentVersion, switchedOn)

/**
 * The pre-release of the agent that [version] waits for, on a head unit that runs agent [running]: where the
 * agent's switch is off, no stable agent in [index] runs [version], and one of its pre-releases does. The user is
 * then asked to switch the agent on, since the app never reaches for a pre-release by itself. Null in every other
 * case: no head unit known, an agent that runs [version] already, a stable agent that would, or none at all.
 *
 * Only an agent the running one can update to counts: one above it, whose own range takes it in.
 */
fun agentPrereleaseFor(index: CatalogueIndex, version: PackageVersion, running: String?, switchedOn: Set<String>): PackageVersion? {
    if (running == null || AGENT_PACKAGE_ID in switchedOn || Resolver.suits(version, running)) return null
    val agent = index.packages.find { it.id == AGENT_PACKAGE_ID } ?: return null
    val now = SemVer.parseOrNull(running) ?: return null
    fun provides(candidate: PackageVersion): Boolean {
        val at = SemVer.parseOrNull(candidate.version) ?: return false
        return at > now && Resolver.suits(candidate, running) && Resolver.suits(version, candidate.version)
    }
    if (agent.versions.any { provides(it) }) return null
    // Pre-releases come newest first out of the catalogue parser: the newest that does.
    return agent.prereleases.firstOrNull { provides(it) }
}

/**
 * A version of a package that one agent is too old for and a newer agent is not: what an update of the agent
 * brings within reach. A feature that takes both the agent and another package is had by updating the agent
 * first and then installing this.
 */
data class Unblocked(
    val id: String,
    /** The package's display name. */
    val name: String,
    /** The newest version that the newer agent runs. */
    val version: String,
    /** The version on the head unit. Null when the package is not there. */
    val installed: String?,
) {
    /** A name and a version, such as "App Launcher 0.3.0". */
    val label: String get() = "$name $version"
}

/**
 * What an agent of version [newer] runs and one of [older] does not, on a head unit that has [installed]: of each
 * package of [index] but the agent's own, the newest version that suits [newer], where that is above every
 * version that suits [older] and above the one on the head unit. A package that [older] runs no version of
 * counts, since it could not be installed at all. Installed packages come first, then by name. Each package is
 * judged by its [offeredVersions], as [switchedOn] has its switch.
 */
fun unblockedBy(
    index: CatalogueIndex,
    installed: List<InstalledPackage>,
    older: String,
    newer: String,
    switchedOn: Set<String> = emptySet(),
): List<Unblocked> =
    index.packages.mapNotNull { pkg ->
        if (pkg.id == AGENT_PACKAGE_ID) return@mapNotNull null
        val versions = offeredVersions(pkg, switchedOn)
        val now = versions.firstOrNull { Resolver.suits(it, newer) } ?: return@mapNotNull null
        val reach = SemVer.parseOrNull(now.version) ?: return@mapNotNull null
        val before = versions.firstOrNull { Resolver.suits(it, older) }?.let { SemVer.parseOrNull(it.version) }
        val have = installed.find { it.id == pkg.id }?.version
        val held = have?.let(SemVer::parseOrNull)
        if ((before != null && before >= reach) || (held != null && held >= reach)) return@mapNotNull null
        Unblocked(pkg.id, pkg.name, now.version, have)
    }.sortedWith(compareBy({ it.installed == null }, { it.name.lowercase() }))

/**
 * What the job that ended with [result] brought within reach, on a head unit that now has [installed]: where it
 * updated the agent and that went well, what the new version runs and the one before did not ([unblockedBy]).
 * Empty for every other job.
 */
fun unblockedBy(index: CatalogueIndex, installed: List<InstalledPackage>, result: JobResult, switchedOn: Set<String> = emptySet()): List<Unblocked> {
    if (result.state != JobState.SUCCEEDED) return emptyList()
    val step = result.steps.find { it.id == AGENT_PACKAGE_ID && it.action != "remove" && it.result == "done" } ?: return emptyList()
    return unblockedBy(index, installed, step.oldVersion ?: return emptyList(), step.version, switchedOn)
}

/** A newer agent that the catalogue has for the head unit, and what it would bring. */
data class AgentUpdate(
    /** The version on offer. */
    val version: String,
    /** The version that runs on the head unit. */
    val running: String,
    /** What the running agent is too old for and this one is not: see [unblockedBy]. */
    val unblocks: List<Unblocked>,
    /** [version] is a pre-release of the agent: see [CataloguePackage.isPrerelease]. */
    val prerelease: Boolean = false,
)

/**
 * The update of the agent that [index] has for the head unit of [holdings], with each package's pre-release
 * versions offered as [switchedOn] says, the agent's own among them. Null when there is none, or no head unit.
 */
fun agentUpdate(index: CatalogueIndex, holdings: Holdings?, switchedOn: Set<String> = emptySet()): AgentUpdate? {
    if (holdings == null) return null
    val agent = index.packages.find { it.id == AGENT_PACKAGE_ID } ?: return null
    val status = packageStatus(agent, holdings, switchedOn)?.takeIf { it.updateAvailable } ?: return null
    val newer = status.offered.version
    return AgentUpdate(
        version = newer,
        running = holdings.agentVersion,
        unblocks = unblockedBy(index, holdings.installed, holdings.agentVersion, newer, switchedOn),
        prerelease = status.offeredPrerelease,
    )
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.model.AppVersion
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
 * A package whose pre-release the offered version needs while its switch is off: a dependency, or the agent. The
 * package's page names it and offers to switch it on, and the list's row notes it; nothing is switched on without
 * the user.
 */
data class PrereleaseAsk(
    val id: String,
    /** The catalogue's name for it. */
    val name: String,
    /** The pre-release that would do. */
    val version: String,
)

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
    /**
     * Whether [offered] runs on the head unit's agent, and this app may install it ([Resolver.suits]). True when no
     * head unit is known and this app is not too old for it.
     */
    val suits: Boolean,
    /** The version on the head unit. Null when the package is not there, and when no head unit is known. */
    val installed: String?,
    /**
     * The catalogue's newest version, when the head unit's agent cannot run it or this app is too old for it, and
     * [offered], which is older, is neither: what a newer agent, or a newer app ([heldBackByApp]), would bring.
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
    /**
     * The packages whose pre-releases [offered] needs, with their switches off: dependencies first, then the agent.
     * [offered] cannot go on the head unit until the user switches them on, so it is no update meanwhile. Filled in
     * only by the [packageStatus] that is given the catalogue.
     */
    val asks: List<PrereleaseAsk> = emptyList(),
    /**
     * [offered] does not [suits] because this app is too old for it: its `requires.app` leaves [AppVersion] out.
     * Then no version of the package is offered at all.
     */
    val needsApp: Boolean = false,
    /** [heldBack] is held back by this app's version alone: the head unit's agent runs it, and this app is too old. */
    val heldBackByApp: Boolean = false,
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

    /**
     * The head unit has an older version than [offered], its agent runs [offered], and [offered] waits for no other
     * package's pre-release ([asks]).
     */
    val updateAvailable: Boolean get() = installed != null && suits && !upToDate && asks.isEmpty()
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
 *
 * What the offered version waits for in other packages is not looked at here, since that takes the catalogue:
 * the screens take a package's status from the [packageStatus] that is given it.
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
    val heldBack = newest.takeIf { suitable != null && suitable !== newest }
    return PackageStatus(
        offered = offered,
        suits = suitable != null,
        installed = have,
        heldBack = heldBack,
        offeredPrerelease = pkg.isPrerelease(offered.version),
        installedPrerelease = have != null && pkg.isPrerelease(have),
        needsSwitch = needsSwitch,
        needsApp = !AppVersion.takes(offered.requires.app),
        heldBackByApp = heldBack != null && !AppVersion.takes(heldBack.requires.app) && Resolver.suitsAgent(heldBack, agentVersion),
    )
}

/**
 * The status of [pkg] of [index] on the head unit of [holdings] (null when none is known), with what its offered
 * version waits for: the pre-releases of other packages it needs, whose switches among [switchedOn] are off
 * ([PackageStatus.asks]). The list, its counts, a package's page, the agent's card and what a newer agent brings
 * all take it from here.
 */
fun packageStatus(index: CatalogueIndex, pkg: CataloguePackage, holdings: Holdings?, switchedOn: Set<String> = emptySet()): PackageStatus? =
    resolvedStatus(index, pkg, holdings?.installed.orEmpty(), holdings?.agentVersion, switchedOn)?.status

/** A package's [PackageStatus], with the [Resolver.Resolution] of its offered version that its asks come from. */
class ResolvedStatus(
    val status: PackageStatus,
    /**
     * What installing [PackageStatus.offered] takes on the head unit: with no link, as it was the last time. Null
     * for a version that waits for the package's own switch ([PackageStatus.needsSwitch]), of which nothing is
     * worked out.
     */
    val resolution: Resolver.Resolution?,
)

/**
 * The [packageStatus] of [pkg] of [index] on a head unit that has [installed] and runs agent [agentVersion], with
 * the resolution it was worked out from, for a screen that shows what that takes as well.
 */
fun resolvedStatus(
    index: CatalogueIndex,
    pkg: CataloguePackage,
    installed: List<InstalledPackage>,
    agentVersion: String?,
    switchedOn: Set<String>,
): ResolvedStatus? {
    val status = packageStatus(pkg, installed, agentVersion, switchedOn) ?: return null
    if (status.needsSwitch) return ResolvedStatus(status, null)
    val resolution = Resolver.resolve(index, Resolver.Needed(pkg, status.offered), installed, agentVersion, switchedOn)
    return ResolvedStatus(status.copy(asks = prereleaseAsks(index, status.offered, resolution, agentVersion, switchedOn)), resolution)
}

/**
 * What [version] waits for on a head unit that runs agent [running], by its [resolution] there: the packages whose
 * pre-releases it needs while their switches among [switchedOn] are off. Its dependencies' come first, then the
 * agent's: where only a pre-release agent runs [version] itself ([agentPrereleaseFor]), or a dependency of it
 * ([Resolver.Resolution.needsPrerelease]). Each package is asked for once. Empty where [version] can be installed
 * as the switches are.
 */
fun prereleaseAsks(index: CatalogueIndex, version: PackageVersion, resolution: Resolver.Resolution, running: String?, switchedOn: Set<String>): List<PrereleaseAsk> {
    val (agent, dependencies) = resolution.needsPrerelease.partition { it.id == AGENT_PACKAGE_ID }
    val own = agentPrereleaseFor(index, version, running, switchedOn)
    val needs = dependencies.map { it.id to it.version } + listOfNotNull(own?.let { AGENT_PACKAGE_ID to it.version }) + agent.map { it.id to it.version }
    return needs.distinctBy { it.first }.map { (id, at) -> PrereleaseAsk(id, nameOf(index, id), at) }
}

private fun nameOf(index: CatalogueIndex, id: String): String = index.packages.find { it.id == id }?.name ?: id

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
 *
 * The version is the one the package's status ([resolvedStatus]) offers on [newer], so that this says what the
 * list and the package's page will: one that waits for another package's pre-release ([PackageStatus.asks]), or
 * for the package's own switch, is not brought within reach.
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
        val status = resolvedStatus(index, pkg, installed, newer, switchedOn)?.status ?: return@mapNotNull null
        if (!status.suits || status.needsSwitch || status.asks.isNotEmpty()) return@mapNotNull null
        val now = status.offered
        val versions = offeredVersions(pkg, switchedOn)
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
    val status = packageStatus(index, agent, holdings, switchedOn)?.takeIf { it.updateAvailable } ?: return null
    val newer = status.offered.version
    return AgentUpdate(
        version = newer,
        running = holdings.agentVersion,
        unblocks = unblockedBy(index, holdings.installed, holdings.agentVersion, newer, switchedOn),
        prerelease = status.offeredPrerelease,
    )
}

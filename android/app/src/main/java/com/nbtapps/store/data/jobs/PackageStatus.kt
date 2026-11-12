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
 * Where a catalogue package stands against a head unit: the one rule the catalogue's list, a package's page and
 * the agent's update card all show, so that they agree. The agent has the last word when a job is planned.
 */
data class PackageStatus(
    /**
     * The version on offer: the newest that runs on the head unit's agent. With none that does, or with no head
     * unit known, the newest of all.
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
) {
    /** [offered] is what the head unit already has, or older than it. */
    val upToDate: Boolean
        get() {
            val have = installed?.let(SemVer::parseOrNull) ?: return false
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
 * known, an empty list and null. Null for a package that has no version.
 */
fun packageStatus(pkg: CataloguePackage, installed: List<InstalledPackage>, agentVersion: String?): PackageStatus? {
    // Versions come newest first out of the catalogue parser.
    val newest = pkg.latest ?: return null
    val suitable = pkg.versions.firstOrNull { Resolver.suits(it, agentVersion) }
    return PackageStatus(
        offered = suitable ?: newest,
        suits = suitable != null,
        installed = installedVersion(pkg.id, installed, agentVersion),
        heldBack = newest.takeIf { suitable != null && suitable !== newest },
    )
}

fun packageStatus(pkg: CataloguePackage, holdings: Holdings?): PackageStatus? =
    packageStatus(pkg, holdings?.installed.orEmpty(), holdings?.agentVersion)

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
 * counts, since it could not be installed at all. Installed packages come first, then by name.
 */
fun unblockedBy(index: CatalogueIndex, installed: List<InstalledPackage>, older: String, newer: String): List<Unblocked> =
    index.packages.mapNotNull { pkg ->
        if (pkg.id == AGENT_PACKAGE_ID) return@mapNotNull null
        // Versions come newest first out of the catalogue parser.
        val now = pkg.versions.firstOrNull { Resolver.suits(it, newer) } ?: return@mapNotNull null
        val reach = SemVer.parseOrNull(now.version) ?: return@mapNotNull null
        val before = pkg.versions.firstOrNull { Resolver.suits(it, older) }?.let { SemVer.parseOrNull(it.version) }
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
fun unblockedBy(index: CatalogueIndex, installed: List<InstalledPackage>, result: JobResult): List<Unblocked> {
    if (result.state != JobState.SUCCEEDED) return emptyList()
    val step = result.steps.find { it.id == AGENT_PACKAGE_ID && it.action != "remove" && it.result == "done" } ?: return emptyList()
    return unblockedBy(index, installed, step.oldVersion ?: return emptyList(), step.version)
}

/** A newer agent that the catalogue has for the head unit, and what it would bring. */
data class AgentUpdate(
    /** The version on offer. */
    val version: String,
    /** The version that runs on the head unit. */
    val running: String,
    /** What the running agent is too old for and this one is not: see [unblockedBy]. */
    val unblocks: List<Unblocked>,
)

/** The update of the agent that [index] has for the head unit of [holdings]. Null when there is none, or no head unit. */
fun agentUpdate(index: CatalogueIndex, holdings: Holdings?): AgentUpdate? {
    if (holdings == null) return null
    val agent = index.packages.find { it.id == AGENT_PACKAGE_ID } ?: return null
    val status = packageStatus(agent, holdings)?.takeIf { it.updateAvailable } ?: return null
    val newer = status.offered.version
    return AgentUpdate(
        version = newer,
        running = holdings.agentVersion,
        unblocks = unblockedBy(index, holdings.installed, holdings.agentVersion, newer),
    )
}

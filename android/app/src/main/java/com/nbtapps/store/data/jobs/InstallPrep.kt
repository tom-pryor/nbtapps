// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageVersion

/** What a job that installs a version takes, as the phone works it out before it asks the agent for a plan. */
internal class InstallPrep(
    /** The archives to offer: dependencies ahead of what depends on them, the target last. Empty with [waiting]. */
    val needed: List<Resolver.Needed>,
    /** Why the job stops here, before anything is downloaded or planned. Null where it goes on. */
    val waiting: String?,
)

/**
 * What installing [version] of [pkg] of [index] takes on a head unit that has [installed] and runs agent
 * [agentVersion]: the phone's own resolution, to know what to offer. The agent resolves again and has the last
 * word. A dependency is taken in a pre-release only where its own switch among [switchedOn] is on.
 *
 * Where the version waits for another package's pre-release ([prereleaseAsks]), as the package's page and the
 * list say it does, the job goes no further: the agent could only refuse it, and the user is told which
 * package's pre-release versions to turn on.
 */
internal fun installPrep(
    index: CatalogueIndex,
    pkg: CataloguePackage,
    version: PackageVersion,
    installed: List<InstalledPackage>,
    agentVersion: String?,
    switchedOn: Set<String>,
): InstallPrep {
    val resolution = Resolver.resolve(index, Resolver.Needed(pkg, version), installed, agentVersion, switchedOn)
    val asks = prereleaseAsks(index, version, resolution, agentVersion, switchedOn)
    return if (asks.isEmpty()) InstallPrep(resolution.needed, null) else InstallPrep(emptyList(), waitingFor(asks))
}

/**
 * Why a job for a version that waits for [asks] stops, in the words of the package's page: "Needs NBT Runtime
 * 2.2.0-beta.1, which is a pre-release.", and what to do about it.
 */
internal fun waitingFor(asks: List<PrereleaseAsk>): String {
    val needs = andList(asks.map { "${it.name} ${it.version}" })
    val names = andList(asks.map { it.name })
    val which = if (asks.size == 1) "which is a pre-release" else "which are pre-releases"
    return "Needs $needs, $which. To install this version, turn on pre-release versions of $names first."
}

/** "A", "A and B", "A, B and C". */
private fun andList(items: List<String>): String =
    if (items.size < 2) items.joinToString() else items.dropLast(1).joinToString() + " and " + items.last()

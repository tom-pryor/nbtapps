// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.MissingDependency
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.SemVer
import com.nbtapps.store.model.VersionRange

/**
 * The phone's own dependency resolution. The agent resolves dependencies itself and has the last word; the
 * phone runs the same rules only to decide which archives to download and to offer, so that `job.plan` fits
 * in one frame and the user is not sent to the car to find out that something is missing.
 */
object Resolver {
    /** One archive a job needs. */
    data class Needed(val pkg: CataloguePackage, val version: PackageVersion)

    class Resolution(
        /** The archives to offer: dependencies ahead of what depends on them, the target last. */
        val needed: List<Needed>,
        /** Dependencies the catalogue cannot satisfy for this head unit. */
        val unavailable: List<MissingDependency>,
    )

    /**
     * Works out what installing [target] takes on a head unit with [installed] on it.
     *
     * A dependency is left alone when an installed package satisfies it. Otherwise the highest version in
     * range is taken that suits [agentVersion] (any, if that is not known) and is not below what is installed.
     */
    fun resolve(
        index: CatalogueIndex,
        target: Needed,
        installed: List<InstalledPackage>,
        agentVersion: String?,
    ): Resolution {
        val chosen = LinkedHashMap<String, Needed>()
        val unavailable = ArrayList<MissingDependency>()
        val visiting = HashSet<String>()

        fun visit(item: Needed) {
            // A cycle is the agent's to refuse; here it must only not loop.
            if (!visiting.add(item.pkg.id)) return
            for (dependency in item.version.depends) {
                val have = chosen[dependency.id]?.version?.version ?: installed.find { it.id == dependency.id }?.version
                if (have != null && VersionRange.matches(have, dependency.version)) continue
                val installedVersion = installed.find { it.id == dependency.id }?.version?.let(SemVer::parseOrNull)
                val pkg = index.packages.find { it.id == dependency.id }
                // Versions come newest first out of the catalogue parser.
                val pick = pkg?.versions?.firstOrNull { candidate ->
                    val version = SemVer.parseOrNull(candidate.version) ?: return@firstOrNull false
                    VersionRange.matches(candidate.version, dependency.version) &&
                        suits(candidate, agentVersion) &&
                        (installedVersion == null || version >= installedVersion)
                }
                if (pkg == null || pick == null) {
                    unavailable += MissingDependency(dependency.id, dependency.version, item.pkg.id)
                    continue
                }
                val needed = Needed(pkg, pick)
                visit(needed)
                chosen[dependency.id] = needed
            }
            visiting.remove(item.pkg.id)
        }

        visit(target)
        chosen.remove(target.pkg.id)
        return Resolution(chosen.values.toList() + target, unavailable)
    }

    /**
     * Whether [version] runs on an agent of [agentVersion]: see [PackageVersion.agentRange]. An unknown agent
     * version rules nothing out.
     */
    fun suits(version: PackageVersion, agentVersion: String?): Boolean =
        agentVersion == null || VersionRange.matches(agentVersion, version.agentRange)
}

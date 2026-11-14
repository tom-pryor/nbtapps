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

    /**
     * A dependency that only a pre-release of package [id] meets, while that package's switch is off: [version]
     * is the pre-release that would be taken, and [requiredBy] the package that needs it. The user is asked to
     * switch [id] on; the app never takes a pre-release for a dependency by itself.
     */
    data class PrereleaseNeed(val id: String, val version: String, val requiredBy: String)

    class Resolution(
        /** The archives to offer: dependencies ahead of what depends on them, the target last. */
        val needed: List<Needed>,
        /** Dependencies the catalogue cannot satisfy for this head unit. */
        val unavailable: List<MissingDependency>,
        /** Dependencies a pre-release would satisfy, were its package switched on. Not among [unavailable]. */
        val needsPrerelease: List<PrereleaseNeed> = emptyList(),
    )

    /**
     * Works out what installing [target] takes on a head unit with [installed] on it.
     *
     * A dependency is left alone when an installed package satisfies it. Otherwise the highest version in
     * range is taken that suits [agentVersion] (any, if that is not known) and is not below what is installed,
     * of the dependency's [offeredVersions]: its pre-release versions count only where [switchedOn] has it. Where
     * nothing but one of those would do, the dependency is in [Resolution.needsPrerelease].
     */
    fun resolve(
        index: CatalogueIndex,
        target: Needed,
        installed: List<InstalledPackage>,
        agentVersion: String?,
        switchedOn: Set<String> = emptySet(),
    ): Resolution {
        val chosen = LinkedHashMap<String, Needed>()
        val unavailable = ArrayList<MissingDependency>()
        val needsPrerelease = ArrayList<PrereleaseNeed>()
        val visiting = HashSet<String>()

        fun visit(item: Needed) {
            // A cycle is the agent's to refuse; here it must only not loop.
            if (!visiting.add(item.pkg.id)) return
            for (dependency in item.version.depends) {
                val have = chosen[dependency.id]?.version?.version ?: installed.find { it.id == dependency.id }?.version
                if (have != null && VersionRange.matches(have, dependency.version)) continue
                val installedVersion = installed.find { it.id == dependency.id }?.version?.let(SemVer::parseOrNull)
                val pkg = index.packages.find { it.id == dependency.id }
                val on = dependency.id in switchedOn
                val pick = pkg?.let { pick(it, on, dependency.version, agentVersion, installedVersion) }
                if (pkg == null || pick == null) {
                    val prerelease = pkg?.takeIf { !on }?.let { pick(it, true, dependency.version, agentVersion, installedVersion) }
                    if (prerelease != null) {
                        needsPrerelease += PrereleaseNeed(dependency.id, prerelease.version, item.pkg.id)
                    } else {
                        unavailable += MissingDependency(dependency.id, dependency.version, item.pkg.id)
                    }
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
        return Resolution(chosen.values.toList() + target, unavailable, needsPrerelease)
    }

    /**
     * The version of [pkg] to take for a dependency on [range]: the highest of its [offeredVersions], by
     * [prerelease], that is in range, suits [agentVersion] and is not below [installed]. Null where none is.
     */
    private fun pick(pkg: CataloguePackage, prerelease: Boolean, range: String?, agentVersion: String?, installed: SemVer?): PackageVersion? =
        // Versions come newest first out of offeredVersions.
        offeredVersions(pkg, prerelease).firstOrNull { candidate ->
            val version = SemVer.parseOrNull(candidate.version) ?: return@firstOrNull false
            VersionRange.matches(candidate.version, range) &&
                suits(candidate, agentVersion) &&
                (installed == null || version >= installed)
        }

    /**
     * Whether [version] runs on an agent of [agentVersion]: see [PackageVersion.agentRange]. An unknown agent
     * version rules nothing out.
     */
    fun suits(version: PackageVersion, agentVersion: String?): Boolean =
        agentVersion == null || VersionRange.matches(agentVersion, version.agentRange)
}

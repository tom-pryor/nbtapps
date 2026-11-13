// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

import kotlinx.serialization.Serializable
import kotlinx.serialization.Transient

/**
 * A catalogue source's `v1/index.json`: every package and version it publishes.
 *
 * Packages from outside developers are published apart from the source's own, under `community`, and the parser
 * puts them in [packages] with [CataloguePackage.community] set: see [CatalogueParser][com.nbtapps.store.data.catalogue.CatalogueParser].
 */
@Serializable
data class CatalogueIndex(
    val schema: Int,
    val source: CatalogueSourceInfo,
    /** Increases with every publish. An index with a lower serial than the one already held is rejected. */
    val serial: Long,
    val generated: String? = null,
    val packages: List<CataloguePackage> = emptyList(),
    /**
     * The newest release of this app, which the official catalogue offers so that the app can update itself: it
     * is in no app store. Null when the index names none, or names one that cannot be used.
     */
    val app: AppRelease? = null,
)

/**
 * A release of this app as the index names it under `app`. Everything here is the catalogue's word for what is
 * in the APK at [url]; the app reads the file it downloads before it believes any of it ([versionCode], and
 * the certificate behind [certSha256]), and Android takes no update that the installed app's key did not sign.
 */
@Serializable
data class AppRelease(
    /** What Android compares: an update is a release whose code is above the installed app's. */
    val versionCode: Long,
    val versionName: String,
    val released: String? = null,
    val url: String,
    val size: Long,
    /** SHA-256 of the APK, in hex. */
    val sha256: String,
    /** SHA-256 of the certificate the APK is signed with, in hex. */
    val certSha256: String,
    /** The lowest Android API level the release runs on. */
    val minSdk: Int,
    /** What is new, a line each. */
    val notes: List<String> = emptyList(),
)

@Serializable
data class CatalogueSourceInfo(
    val id: String,
    val name: String,
)

@Serializable
data class CataloguePackage(
    val id: String,
    val name: String,
    val summary: String = "",
    val author: String = "",
    val category: String = "",
    /** Path of the list icon, relative to the source's `v1/` folder. */
    val icon: String? = null,
    /** Path of the [PackageDetails] file, relative to the source's `v1/` folder. */
    val details: String? = null,
    /** Newest first once the index has been through the parser. */
    val versions: List<PackageVersion> = emptyList(),
    /**
     * From an outside developer: published under the index's `community`, reviewed and countersigned by the
     * source, with each version's archive at the developer's own address. Set by the parser only, never read
     * from the file.
     */
    @Transient val community: Boolean = false,
) {
    val latest: PackageVersion? get() = versions.firstOrNull()
}

@Serializable
data class PackageVersion(
    val version: String,
    val released: String? = null,
    val url: String,
    val size: Long,
    /** SHA-256 of the archive file, in hex. */
    val sha256: String,
    /**
     * SHA-256 of the exact bytes of the `manifest.json` inside the archive, in hex. The manifest in turn
     * lists a hash for every other file, so the signed index vouches for each file on its own.
     */
    val manifestSha256: String,
    val requires: PackageRequirements = PackageRequirements(),
    val depends: List<PackageDependency> = emptyList(),
    /** Risk flags as published. Unknown values are kept; see [RiskFlag] for the ones the app explains. */
    val flags: List<String> = emptyList(),
    /**
     * The source's own signature over the archive's `manifest.json`, 128 hex characters, for a version of a
     * [community][CataloguePackage.community] package: the archive's `manifest.sig` is its author's, and this is
     * checked in its place, by the app and by the agent. Null for every other version.
     */
    val signature: String? = null,
) {
    val riskFlags: List<RiskFlag> get() = flags.mapNotNull(RiskFlag::fromId)

    /** The archive is to be checked against [signature], not its own `manifest.sig`. */
    val detachedSignature: Boolean get() = signature != null

    /**
     * The agent versions this version installs on: [requires], and for one with a [detachedSignature], no agent
     * below [DETACHED_SIGNATURE_AGENT], since an older one would check the archive's own `manifest.sig`. A lower
     * bound that the second makes needless is left out. Null where any agent will do.
     */
    val agentRange: String?
        get() {
            val published = requires.agent?.trim()?.takeIf { it.isNotEmpty() }
            if (!detachedSignature) return published
            val floor = SemVer.parseOrNull(DETACHED_SIGNATURE_AGENT)!!
            val parts = published?.split(Regex("\\s+")).orEmpty()
            fun bound(part: String, op: String) = if (part.startsWith(op)) SemVer.parseOrNull(part.drop(op.length)) else null
            // A part that already keeps out every agent below the floor.
            val enough = parts.any { part ->
                val at = bound(part, ">=") ?: bound(part, ">") ?: (if (part.first().isDigit()) bound(part, "") else bound(part, "="))
                at != null && at >= floor
            }
            if (enough) return published
            val kept = parts.filterNot { part -> (bound(part, ">=") ?: bound(part, ">"))?.let { it < floor } == true }
            return (listOf(">=$DETACHED_SIGNATURE_AGENT") + kept).joinToString(" ")
        }

    companion object {
        /** The first agent that installs a package with the catalogue's detached signature. */
        const val DETACHED_SIGNATURE_AGENT = "1.2.0"
    }
}

@Serializable
data class PackageRequirements(
    /** Semver range on the head-unit agent version, such as `>=1.0.0`. */
    val agent: String? = null,
)

@Serializable
data class PackageDependency(
    val id: String,
    /** Semver range on the dependency's version. Null accepts any version. */
    val version: String? = null,
)

enum class RiskFlag(val id: String, val label: String, val explanation: String) {
    RUNS_SCRIPTS("runs-scripts", "Runs scripts", "Runs its own scripts on the head unit during install or removal."),
    MODIFIES_SYSTEM("modifies-system", "Modifies system files", "Writes outside its own folders or replaces stock head-unit files."),
    NEEDS_REBOOT("needs-reboot", "Needs restart", "The head unit or its HMI restarts to finish the install.");

    companion object {
        fun fromId(id: String): RiskFlag? = entries.find { it.id == id }
    }
}

/** A package's `v1/details/<id>.json`: the long-form listing, fetched when the detail screen opens. */
@Serializable
data class PackageDetails(
    val id: String,
    val description: String = "",
    val homepage: String? = null,
    val screenshots: List<String> = emptyList(),
    val changelog: List<ChangelogEntry> = emptyList(),
)

@Serializable
data class ChangelogEntry(
    val version: String,
    val notes: List<String> = emptyList(),
)

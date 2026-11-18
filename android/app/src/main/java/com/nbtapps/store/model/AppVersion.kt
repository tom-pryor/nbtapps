// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

/**
 * This app's own version, as a package's `requires.app` ([PackageRequirements.app]) is held against it: the one place
 * that gives it. The name of a trial build ends in [TRIAL_SUFFIX] (`app/build.gradle.kts`), which is left off, so
 * that a trial of a version counts as that version and not as a pre-release before it.
 *
 * The app sets [current] as it starts, from what Android says of the copy that runs, and a test sets its own. While
 * it is null, nothing is ruled out, as nothing is for a head unit whose agent's version is not known.
 */
object AppVersion {
    const val TRIAL_SUFFIX = "-trial"

    @Volatile
    var current: String? = null

    /** [versionName], Android's name of a copy of the app, as a version: without [TRIAL_SUFFIX], and null if no semver. */
    fun of(versionName: String): String? = versionName.removeSuffix(TRIAL_SUFFIX).takeIf { SemVer.parseOrNull(it) != null }

    /** Whether this app is one that [range], a package's `requires.app`, takes in. No range takes every version. */
    fun takes(range: String?): Boolean {
        val version = current ?: return true
        return VersionRange.matches(version, range)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.update

import com.nbtapps.store.model.AppRelease

/** The copy of the app that is running, as far as an update of it is concerned. */
data class InstalledApp(
    val packageName: String,
    val versionCode: Long,
    val versionName: String,
    /** SHA-256 of the certificate this copy is signed with, in hex. Null when Android would not say. */
    val certSha256: String?,
    /** The Android API level of the phone. */
    val sdk: Int,
)

/** A release of the app that is newer than the copy that runs. */
sealed interface AppUpdateOffer {
    val release: AppRelease

    /** This copy can take it. */
    data class Available(override val release: AppRelease) : AppUpdateOffer

    /** It is out, and this copy cannot take it. */
    data class Blocked(override val release: AppRelease, val reason: Reason) : AppUpdateOffer

    enum class Reason {
        /**
         * This copy is signed with another key than the release: a trial or a debug build. Android takes an update
         * only from the key the installed app was signed with, so this copy has to be uninstalled first.
         */
        OTHER_KEY,

        /** The release needs a newer Android than the phone has. */
        ANDROID_TOO_OLD,
    }
}

/**
 * What the catalogue's [release] of the app is to the copy that runs: nothing, when there is none or it is not
 * newer; an update; or a newer release this copy cannot take.
 *
 * Only the version code decides what is newer, as it does for Android. The key comes before the Android
 * version: a copy signed with another key cannot be updated on any phone.
 */
fun appUpdate(release: AppRelease?, installed: InstalledApp): AppUpdateOffer? = when {
    release == null || release.versionCode <= installed.versionCode -> null
    installed.certSha256 != release.certSha256 -> AppUpdateOffer.Blocked(release, AppUpdateOffer.Reason.OTHER_KEY)
    installed.sdk < release.minSdk -> AppUpdateOffer.Blocked(release, AppUpdateOffer.Reason.ANDROID_TOO_OLD)
    else -> AppUpdateOffer.Available(release)
}

/** What a downloaded APK says of itself, read out of the file. */
data class ApkFacts(
    val packageName: String,
    val versionCode: Long,
    /** SHA-256 of the certificate it is signed with, in hex. Null when it could not be read. */
    val certSha256: String?,
)

/**
 * Why a downloaded file is not the update the catalogue described, or null when it is. [facts] is what was read
 * out of the file; null means it could not be read as an APK at all.
 *
 * This is looked at before Android is asked, so that a wrong file ends in words and not in a system dialog.
 * Android checks the signature itself whatever this says: a certificate that could not be read here is let
 * through for that reason, and one that was read has to be this copy's own.
 */
fun apkProblem(facts: ApkFacts?, release: AppRelease, installed: InstalledApp): String? = when {
    facts == null -> "The download isn't an Android app."
    facts.packageName != installed.packageName -> "The download is another app (${facts.packageName})."
    facts.versionCode != release.versionCode ->
        "The download is version code ${facts.versionCode}, not the ${release.versionCode} the catalogue names."
    facts.certSha256 != null && facts.certSha256 != installed.certSha256 ->
        "The download is signed with another key than this app."
    else -> null
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.whatsnew

import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.model.AppRelease

/** What the app does about the notes of the version that runs, at a start and whenever a catalogue comes in. */
sealed interface WhatsNewStep {
    /** They have been seen, or were never to be shown: nothing to do. */
    data object Nothing : WhatsNewStep

    /** There is nothing to show for this version, and never will be: it is marked as seen without a word. */
    data object MarkSeen : WhatsNewStep

    /** [notes] are shown: kept on the phone, or the catalogue's ([catalogueNotesToKeep] keeps those). */
    data class Show(val notes: VersionNotes) : WhatsNewStep

    /** No notes for this version yet: asked again at the next catalogue, or the next start. */
    data object Wait : WhatsNewStep
}

/**
 * What to do about the notes of the version that runs, from what the phone has [stored], the copy [installed] and the
 * catalogue's [release] of the app, the newest it names (null with no catalogue yet).
 *
 * The notes are shown once in each version, after an update. A fresh install has nothing to tell of, nor has a
 * version older than one already seen, as after a downgrade: those are marked as seen without a word. Kept notes of
 * another version than the one that runs are passed over: they are of an update that was cancelled, or is not done
 * yet.
 */
fun whatsNewStep(stored: WhatsNewSettings, installed: InstalledApp, release: AppRelease?): WhatsNewStep {
    val code = installed.versionCode
    val seen = stored.seen
    val kept = stored.keptFor(code)
    return when {
        seen == code -> WhatsNewStep.Nothing
        (seen == null && installed.freshInstall) || (seen != null && seen > code) -> WhatsNewStep.MarkSeen
        kept != null -> WhatsNewStep.Show(kept)
        release?.versionCode == code && release.notes.isNotEmpty() -> WhatsNewStep.Show(VersionNotes.of(release))
        release?.versionCode == code -> WhatsNewStep.MarkSeen
        else -> WhatsNewStep.Wait
    }
}

/**
 * The notes of the version that runs, where the phone has them, whether or not they have been seen: kept on the phone,
 * or in the catalogue as it is held. Null where it has neither.
 */
fun installedNotes(stored: WhatsNewSettings, installed: InstalledApp, release: AppRelease?): VersionNotes? {
    val code = installed.versionCode
    return stored.keptFor(code)
        ?: release?.takeIf { it.versionCode == code && it.notes.isNotEmpty() }?.let(VersionNotes::of)
}

/**
 * The catalogue's notes of the version that runs, where the phone has none of its own kept: so that they are kept, and
 * [installedNotes] still has them once the catalogue names another version, or none. Null where there is nothing to
 * keep: notes of this version are kept already, or the catalogue's [release] is of another version, has no notes, or
 * is not there. Whether they are shown, or have been seen, is [whatsNewStep]'s to say, and makes no difference here: a
 * fresh install keeps its version's notes as well, though it never shows them.
 */
fun catalogueNotesToKeep(stored: WhatsNewSettings, installed: InstalledApp, release: AppRelease?): VersionNotes? {
    val code = installed.versionCode
    return release?.takeIf { it.versionCode == code && it.notes.isNotEmpty() && stored.keptFor(code) == null }
        ?.let(VersionNotes::of)
}

/** The kept notes of the version [code], where they have lines. */
private fun WhatsNewSettings.keptFor(code: Long): VersionNotes? =
    notes.firstOrNull { it.versionCode == code && it.lines.isNotEmpty() }

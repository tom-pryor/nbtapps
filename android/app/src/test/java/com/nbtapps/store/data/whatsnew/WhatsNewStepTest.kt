// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.whatsnew

import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.data.update.RUNNING
import com.nbtapps.store.data.update.release
import com.nbtapps.store.model.AppRelease
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * When the notes of the version that runs are shown, kept, or passed over ([whatsNewStep], [installedNotes],
 * [catalogueNotesToKeep]), and which notes stay as more are kept ([WhatsNewSettings.keeping]).
 */
class WhatsNewStepTest {

    /** RUNNING is 0.1.0, code 100, installed over an earlier copy. */
    private val updated = RUNNING
    private val fresh = RUNNING.copy(lastUpdateTime = RUNNING.firstInstallTime)

    /** 0.2.0, code 200, installed over this copy. */
    private val next = RUNNING.copy(versionCode = 200, versionName = "0.2.0", lastUpdateTime = RUNNING.lastUpdateTime + 60_000)

    private val thisRelease = release("0.1.0", 100).copy(notes = listOf("Settings", "What's new"))
    private val thisNotes = VersionNotes(100, "0.1.0", "2026-11-12", listOf("Settings", "What's new"))
    private val kept = VersionNotes(100, "0.1.0", "2026-11-12", listOf("Kept before the update"))
    private val otherKept = VersionNotes(200, "0.2.0", null, listOf("An update that was not done"))

    private data class Row(
        val what: String,
        val stored: WhatsNewSettings,
        val release: AppRelease?,
        val step: WhatsNewStep,
        val installed: InstalledApp = RUNNING,
    )

    private val table = listOf(
        Row("seen already", WhatsNewSettings(seen = 100, notes = listOf(kept)), thisRelease, WhatsNewStep.Nothing),
        Row("seen already, as a fresh install", WhatsNewSettings(seen = 100), thisRelease, WhatsNewStep.Nothing, fresh),
        Row("a fresh install, with nothing seen", WhatsNewSettings(), thisRelease, WhatsNewStep.MarkSeen, fresh),
        Row("a fresh install, with no catalogue", WhatsNewSettings(), null, WhatsNewStep.MarkSeen, fresh),
        Row("a fresh install, with its version's notes kept and nothing seen", WhatsNewSettings(notes = listOf(kept)), thisRelease, WhatsNewStep.MarkSeen, fresh),
        Row("a fresh install, with its version's notes kept and no catalogue", WhatsNewSettings(notes = listOf(kept)), null, WhatsNewStep.MarkSeen, fresh),
        Row("a fresh install seen, with its version's notes kept", WhatsNewSettings(seen = 100, notes = listOf(kept)), null, WhatsNewStep.Nothing, fresh),
        Row("a downgrade", WhatsNewSettings(seen = 200, notes = listOf(kept)), thisRelease, WhatsNewStep.MarkSeen),
        Row("a downgrade, as a fresh install", WhatsNewSettings(seen = 200), null, WhatsNewStep.MarkSeen, fresh),
        Row("kept notes", WhatsNewSettings(seen = 90, notes = listOf(kept)), null, WhatsNewStep.Show(kept)),
        Row("kept notes before the catalogue's", WhatsNewSettings(seen = 90, notes = listOf(kept)), thisRelease, WhatsNewStep.Show(kept)),
        Row("kept notes, from a copy that kept nothing seen", WhatsNewSettings(notes = listOf(kept)), null, WhatsNewStep.Show(kept)),
        Row("the catalogue's notes", WhatsNewSettings(seen = 90), thisRelease, WhatsNewStep.Show(thisNotes)),
        Row("the catalogue's notes, nothing seen", WhatsNewSettings(), thisRelease, WhatsNewStep.Show(thisNotes)),
        Row("kept notes of another version", WhatsNewSettings(seen = 90, notes = listOf(otherKept)), thisRelease, WhatsNewStep.Show(thisNotes)),
        Row("kept notes with no lines", WhatsNewSettings(seen = 90, notes = listOf(kept.copy(lines = emptyList()))), thisRelease, WhatsNewStep.Show(thisNotes)),
        Row("the catalogue's release with no notes", WhatsNewSettings(seen = 90), thisRelease.copy(notes = emptyList()), WhatsNewStep.MarkSeen),
        Row("no catalogue yet", WhatsNewSettings(seen = 90), null, WhatsNewStep.Wait),
        Row("no catalogue yet, nothing seen", WhatsNewSettings(), null, WhatsNewStep.Wait),
        Row("the catalogue names a newer release", WhatsNewSettings(seen = 90), release("0.2.0", 200), WhatsNewStep.Wait),
        Row("the catalogue names an older release", WhatsNewSettings(seen = 90), release("0.0.9", 9), WhatsNewStep.Wait),
        Row("kept notes of another version, and no catalogue", WhatsNewSettings(seen = 90, notes = listOf(otherKept)), null, WhatsNewStep.Wait),
        Row("kept notes beside those of an update not done", WhatsNewSettings(seen = 90, notes = listOf(kept, otherKept)), release("0.2.0", 200), WhatsNewStep.Show(kept)),
        Row("kept notes after those of an update not done", WhatsNewSettings(seen = 90, notes = listOf(otherKept, kept)), null, WhatsNewStep.Show(kept)),
        Row("seen already, beside the notes of an update not done", WhatsNewSettings(seen = 100, notes = listOf(kept, otherKept)), null, WhatsNewStep.Nothing),
        Row("the notes kept by the copy before, as the new copy", WhatsNewSettings(seen = 100, notes = listOf(kept, otherKept)), null, WhatsNewStep.Show(otherKept), next),
    )

    @Test
    fun `the step for each case`() {
        for (row in table) {
            assertEquals(row.what, row.step, whatsNewStep(row.stored, row.installed, row.release))
        }
    }

    @Test
    fun `the notes the phone has for the version that runs, seen or not`() {
        assertEquals(kept, installedNotes(WhatsNewSettings(seen = 100, notes = listOf(kept)), updated, thisRelease))
        assertEquals(thisNotes, installedNotes(WhatsNewSettings(seen = 100), fresh, thisRelease))
        assertEquals(thisNotes, installedNotes(WhatsNewSettings(notes = listOf(otherKept)), updated, thisRelease))
        assertEquals(thisNotes, installedNotes(WhatsNewSettings(notes = listOf(kept.copy(lines = emptyList()))), updated, thisRelease))
        assertEquals(null, installedNotes(WhatsNewSettings(notes = listOf(otherKept)), updated, null))
        assertEquals(null, installedNotes(WhatsNewSettings(), updated, release("0.2.0", 200)))
        assertEquals(null, installedNotes(WhatsNewSettings(), updated, thisRelease.copy(notes = emptyList())))
        // Beside the notes of an update that did not happen, with the catalogue naming that update's release or none.
        assertEquals(kept, installedNotes(WhatsNewSettings(seen = 100, notes = listOf(kept, otherKept)), updated, release("0.2.0", 200)))
        assertEquals(kept, installedNotes(WhatsNewSettings(seen = 100, notes = listOf(otherKept, kept)), updated, null))
    }

    @Test
    fun `keeping notes leaves those of the version that runs, and of one more at most`() {
        val third = VersionNotes(300, "0.3.0", null, listOf("The update after"))
        // The catalogue's notes of the version that runs.
        assertEquals(WhatsNewSettings(seen = 90, notes = listOf(thisNotes)), WhatsNewSettings(seen = 90).keeping(thisNotes, 100))
        // An update is started: the notes of the version that runs stay.
        assertEquals(WhatsNewSettings(seen = 100, notes = listOf(kept, otherKept)), WhatsNewSettings(seen = 100, notes = listOf(kept)).keeping(otherKept, 100))
        // It did not happen, and another is started: the notes of the first are dropped.
        assertEquals(listOf(kept, third), WhatsNewSettings(notes = listOf(kept, otherKept)).keeping(third, 100).notes)
        // The same release again takes the place of its notes.
        val again = otherKept.copy(lines = listOf("Asked again"))
        assertEquals(listOf(kept, again), WhatsNewSettings(notes = listOf(kept, otherKept)).keeping(again, 100).notes)
        // As the new copy: the notes the copy before kept for it stay, those of the copy before go.
        assertEquals(listOf(otherKept, third), WhatsNewSettings(notes = listOf(kept, otherKept)).keeping(third, 200).notes)
        // The catalogue's notes of the version that runs, beside those of an update not done yet: they stay.
        assertEquals(listOf(otherKept, thisNotes), WhatsNewSettings(notes = listOf(otherKept)).keeping(thisNotes, 100).notes)
        // Only the last of a later version stays, and none of an earlier one.
        val earlier = VersionNotes(90, "0.0.9", null, listOf("Before"))
        assertEquals(listOf(third, thisNotes), WhatsNewSettings(notes = listOf(earlier, otherKept, third)).keeping(thisNotes, 100).notes)
        assertEquals(listOf(thisNotes), WhatsNewSettings(notes = listOf(earlier)).keeping(thisNotes, 100).notes)
        // The catalogue's notes in place of kept ones with no lines.
        assertEquals(listOf(otherKept, thisNotes), WhatsNewSettings(notes = listOf(kept.copy(lines = emptyList()), otherKept)).keeping(thisNotes, 100).notes)
    }

    @Test
    fun `the catalogue's notes of the version that runs are kept, shown or not, where none are kept`() {
        // Kept whether they are to be shown, have been seen, or never will be, as for a fresh install or a downgrade.
        assertEquals(thisNotes, catalogueNotesToKeep(WhatsNewSettings(seen = 90), updated, thisRelease))
        assertEquals(thisNotes, catalogueNotesToKeep(WhatsNewSettings(), updated, thisRelease))
        assertEquals(thisNotes, catalogueNotesToKeep(WhatsNewSettings(seen = 100), updated, thisRelease))
        assertEquals(thisNotes, catalogueNotesToKeep(WhatsNewSettings(), fresh, thisRelease))
        assertEquals(thisNotes, catalogueNotesToKeep(WhatsNewSettings(seen = 100), fresh, thisRelease))
        assertEquals(thisNotes, catalogueNotesToKeep(WhatsNewSettings(seen = 200), updated, thisRelease))
        // Beside the notes of an update not done, or in place of kept ones with no lines.
        assertEquals(thisNotes, catalogueNotesToKeep(WhatsNewSettings(seen = 100, notes = listOf(otherKept)), updated, thisRelease))
        assertEquals(thisNotes, catalogueNotesToKeep(WhatsNewSettings(notes = listOf(kept.copy(lines = emptyList()))), updated, thisRelease))

        // Not where notes of this version are kept already, nor where the catalogue has none of it to give.
        assertEquals(null, catalogueNotesToKeep(WhatsNewSettings(seen = 100, notes = listOf(kept)), updated, thisRelease))
        assertEquals(null, catalogueNotesToKeep(WhatsNewSettings(notes = listOf(otherKept, kept)), fresh, thisRelease))
        assertEquals(null, catalogueNotesToKeep(WhatsNewSettings(seen = 100), updated, release("0.2.0", 200).copy(notes = listOf("Later"))))
        assertEquals(null, catalogueNotesToKeep(WhatsNewSettings(seen = 100), updated, release("0.0.9", 9).copy(notes = listOf("Earlier"))))
        assertEquals(null, catalogueNotesToKeep(WhatsNewSettings(seen = 90), updated, thisRelease.copy(notes = emptyList())))
        assertEquals(null, catalogueNotesToKeep(WhatsNewSettings(), fresh, thisRelease.copy(notes = emptyList())))
        assertEquals(null, catalogueNotesToKeep(WhatsNewSettings(seen = 100), updated, null))
    }
}

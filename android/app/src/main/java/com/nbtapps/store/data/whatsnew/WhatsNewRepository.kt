// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.whatsnew

import androidx.datastore.core.DataStore
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.model.AppRelease
import java.io.IOException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/**
 * What is new in the version of the app that runs, shown once after an update: from the notes the copy before it
 * kept as it handed the update to Android ([saveNotes]), or else from the [catalogue] once it names this version.
 * What has been seen, and the notes, are kept in [store], so that a restart neither shows them again nor needs the
 * catalogue for them; the notes kept for an update stand beside this version's, so that an update that does not
 * happen leaves this version's in place. See [whatsNewStep] for when they are shown.
 *
 * The notes a catalogue brings of this version are kept ([catalogueNotesToKeep]) whether or not they are shown, as on
 * a fresh install, so that What's new in Settings still has them once the catalogue names another version. They are
 * kept, and a version with nothing to show is marked as seen, as soon as that is known, in [scope], whether or not
 * the catalogue's screen is open: the sheet waits for that screen, the notes do not.
 */
class WhatsNewRepository(
    catalogue: CatalogueRepository,
    private val store: DataStore<WhatsNewSettings>,
    private val installed: InstalledApp,
    private val scope: CoroutineScope,
) {
    // A file that cannot be read is taken as empty, as the store's own handler takes one this version cannot read.
    private val stored: Flow<WhatsNewSettings> =
        store.data.catch { e -> if (e is IOException) emit(WhatsNewSettings()) else throw e }

    private val release: Flow<AppRelease?> = catalogue.catalogue.map { it.index?.app }.distinctUntilChanged()

    /** The step for what is held now ([whatsNewStep]); null until the store has been read. */
    internal val step: StateFlow<WhatsNewStep?> =
        combine(stored, release) { settings, app -> whatsNewStep(settings, installed, app) }
            .stateIn(scope, SharingStarted.Eagerly, null)

    /** The notes to show now, once: null where there are none, and until the store has been read. */
    val toShow: StateFlow<VersionNotes?> =
        step.map { (it as? WhatsNewStep.Show)?.notes }.stateIn(scope, SharingStarted.Eagerly, null)

    /** The notes of the version that runs where the phone has them, seen or not ([installedNotes]); null where it has none. */
    val notes: Flow<VersionNotes?> =
        combine(stored, release) { settings, app -> installedNotes(settings, installed, app) }.distinctUntilChanged()

    init {
        scope.launch {
            step.collect { step ->
                if (step == WhatsNewStep.MarkSeen) write { it.copy(seen = installed.versionCode) }
            }
        }
        scope.launch {
            combine(stored, release) { settings, app -> catalogueNotesToKeep(settings, installed, app) }
                .distinctUntilChanged()
                .collect { notes -> if (notes != null) write { it.keeping(notes, installed.versionCode) } }
        }
    }

    /** The notes of the version that runs have been seen: they are not shown again. */
    fun markSeen() {
        scope.launch { write { it.copy(seen = installed.versionCode) } }
    }

    /**
     * Keeps the notes of [release], which the app is about to be updated to, for the new copy to show, beside those of
     * the version that runs, which are still wanted where the update does not happen; the notes of an earlier update
     * that did not happen are dropped. Throws [IOException] when they could not be written.
     */
    suspend fun saveNotes(release: AppRelease) {
        store.updateData { it.keeping(VersionNotes.of(release), installed.versionCode) }
    }

    /** A write that fails is let go: the notes are shown once more, or kept again, at the next start. */
    private suspend fun write(change: (WhatsNewSettings) -> WhatsNewSettings) {
        try {
            store.updateData(change)
        } catch (e: IOException) {
            // Nothing to tell the user.
        }
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.whatsnew

import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.data.update.RUNNING
import com.nbtapps.store.data.update.release
import com.nbtapps.store.model.AppRelease
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.PackageDetails
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.job
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** What is new, joined from the catalogue, the store and the copy that runs ([WhatsNewRepository]). */
class WhatsNewRepositoryTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val file by lazy { File(folder.root, "whats-new.json") }
    private val storeScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val store by lazy { whatsNewStore(file, storeScope) }

    private class HeldCatalogue : CatalogueRepository {
        override val catalogue = MutableStateFlow(CatalogueState())

        override suspend fun refresh() = Unit

        override suspend fun details(packageId: String): Result<PackageDetails> = Result.failure(UnsupportedOperationException())
    }

    private val catalogue = HeldCatalogue()

    /** RUNNING is 0.1.0, code 100, installed over an earlier copy. */
    private val thisRelease = release("0.1.0", 100).copy(notes = listOf("Settings", "What's new"))
    private val thisNotes = VersionNotes.of(thisRelease)

    private fun repository(installed: InstalledApp = RUNNING) = WhatsNewRepository(catalogue, store, installed, scope)

    /** The catalogue comes in, naming [app] as the newest release of the app. */
    private fun fetched(app: AppRelease?) {
        catalogue.catalogue.value = CatalogueState(
            index = CatalogueIndex(schema = 1, source = CatalogueSourceInfo("test", "Test source"), serial = 1, app = app),
        )
    }

    /** The first value that [predicate] takes, which may be null: it is wrapped, so as not to be taken for the time running out. */
    private fun <T> Flow<T>.await(what: String, predicate: (T) -> Boolean): T = runBlocking {
        val found = withTimeoutOrNull(10_000) { listOf(first(predicate)) } ?: throw AssertionError("never came: $what")
        found.single()
    }

    /** What the store holds, once it is as [predicate] wants it. */
    private fun saved(what: String, predicate: (WhatsNewSettings) -> Boolean) = store.data.await(what, predicate)

    @After
    fun tearDown() = runBlocking {
        scope.coroutineContext.job.cancelAndJoin()
        storeScope.coroutineContext.job.cancelAndJoin()
    }

    @Test
    fun `a fetch that names the version that runs brings its notes, which are shown and kept`() {
        runBlocking { store.updateData { it.copy(seen = 90) } }
        val repository = repository()
        assertEquals(WhatsNewStep.Wait, repository.step.await("the step with no catalogue") { it != null })
        assertNull(repository.toShow.value)

        fetched(thisRelease)
        assertEquals(thisNotes, repository.toShow.await("the notes") { it != null })
        assertEquals(WhatsNewSettings(seen = 90, notes = listOf(thisNotes)), saved("the kept notes") { it.notes.isNotEmpty() })
        // Kept, they are shown from the phone's copy, which is the same.
        assertEquals(WhatsNewStep.Show(thisNotes), repository.step.await("the kept step") { it is WhatsNewStep.Show })
        assertEquals(thisNotes, repository.toShow.value)
    }

    @Test
    fun `notes kept before the update are shown with no catalogue`() {
        runBlocking { store.updateData { it.copy(seen = 90, notes = listOf(thisNotes)) } }
        val repository = repository()
        assertEquals(thisNotes, repository.toShow.await("the notes") { it != null })
    }

    @Test
    fun `notes already seen are not shown`() {
        runBlocking { store.updateData { it.copy(seen = 100, notes = listOf(thisNotes)) } }
        val repository = repository()
        fetched(thisRelease)
        repository.step.await("the step with the catalogue") { it == WhatsNewStep.Nothing }
        assertNull(repository.toShow.value)
    }

    @Test
    fun `a catalogue that names another version shows nothing and keeps nothing`() {
        runBlocking { store.updateData { it.copy(seen = 90) } }
        val repository = repository()
        fetched(release("0.2.0", 200))
        // The step is worked out from the catalogue as it is held.
        catalogue.catalogue.await("the catalogue") { it.index != null }
        assertEquals(WhatsNewStep.Wait, repository.step.await("the step") { it != null })
        assertNull(repository.toShow.value)
        assertEquals(WhatsNewSettings(seen = 90), runBlocking { store.data.first() })

        // Notes kept of another version, from an update that did not happen, are passed over too.
        runBlocking { store.updateData { it.copy(notes = listOf(VersionNotes.of(release("0.2.0", 200)))) } }
        repository.step.await("the step with other notes") { it == WhatsNewStep.Wait }
        assertNull(repository.toShow.value)
    }

    @Test
    fun `marking them seen hides them, and keeps them hidden for a later start`() {
        runBlocking { store.updateData { it.copy(seen = 90, notes = listOf(thisNotes)) } }
        val repository = repository()
        repository.toShow.await("the notes") { it != null }

        repository.markSeen()
        assertEquals(100L, saved("the version seen") { it.seen == 100L }.seen)
        repository.toShow.await("no notes") { it == null }

        val later = WhatsNewRepository(catalogue, store, RUNNING, scope)
        fetched(thisRelease)
        assertEquals(WhatsNewStep.Nothing, later.step.await("the later step") { it != null })
        assertNull(later.toShow.value)
    }

    @Test
    fun `a fresh install, and a release with no notes, are marked seen without a word`() {
        val fresh = repository(RUNNING.copy(lastUpdateTime = RUNNING.firstInstallTime))
        assertEquals(100L, saved("the fresh install seen") { it.seen != null }.seen)
        assertNull(fresh.toShow.value)

        runBlocking { store.updateData { WhatsNewSettings(seen = 90) } }
        val updated = repository()
        fetched(thisRelease.copy(notes = emptyList()))
        // Nothing is kept of a release with no notes.
        assertEquals(WhatsNewSettings(seen = 100), saved("the release with no notes seen") { it.seen == 100L })
        assertNull(updated.toShow.value)
    }

    @Test
    fun `the notes for Settings are there whether or not they were seen`() {
        runBlocking { store.updateData { it.copy(seen = 100) } }
        val repository = repository()
        assertNull(repository.notes.await("no notes yet") { true })
        fetched(release("0.2.0", 200))
        catalogue.catalogue.await("the catalogue") { it.index != null }
        assertNull(repository.notes.await("none, with a catalogue that names another version") { true })

        // Seen already, the notes the catalogue brings are kept all the same, and stay once it names no release.
        fetched(thisRelease)
        assertEquals(thisNotes, repository.notes.await("the catalogue's notes") { it != null })
        assertEquals(WhatsNewSettings(seen = 100, notes = listOf(thisNotes)), saved("the kept notes") { it.notes.isNotEmpty() })
        fetched(null)
        catalogue.catalogue.await("the catalogue with no release") { it.index?.app == null }
        assertEquals(thisNotes, repository.notes.await("the kept notes, with a catalogue that names no release") { true })
    }

    @Test
    fun `a fresh install keeps its version's notes, shows nothing, and still has them once the catalogue names another`() {
        val installed = RUNNING.copy(lastUpdateTime = RUNNING.firstInstallTime)
        val repository = repository(installed)
        fetched(thisRelease)
        assertEquals(
            WhatsNewSettings(seen = 100, notes = listOf(thisNotes)),
            saved("seen, and the notes kept") { it.seen != null && it.notes.isNotEmpty() },
        )
        assertEquals(WhatsNewStep.Nothing, repository.step.await("the step once seen") { it == WhatsNewStep.Nothing })
        assertNull(repository.toShow.value)

        fetched(release("0.2.0", 200))
        catalogue.catalogue.await("the newer catalogue") { it.index?.app?.versionCode == 200L }
        assertEquals(thisNotes, repository.notes.await("the notes, with the catalogue naming a newer version") { true })
        catalogue.catalogue.value = CatalogueState()
        assertEquals(thisNotes, repository.notes.await("the notes, with no catalogue") { true })

        // A later start of the same copy, on the same store, shows nothing either.
        val later = WhatsNewRepository(catalogue, store, installed, scope)
        assertEquals(WhatsNewStep.Nothing, later.step.await("the later step") { it != null })
        assertNull(later.toShow.value)
        assertEquals(WhatsNewSettings(seen = 100, notes = listOf(thisNotes)), runBlocking { store.data.first() })
    }

    @Test
    fun `saving a release's notes keeps them for the copy it updates to`() {
        val repository = repository()
        val next = release("0.2.0", 200)
        runBlocking { repository.saveNotes(next) }
        assertEquals(listOf(VersionNotes.of(next)), saved("the notes") { it.notes.isNotEmpty() }.notes)
        // They are not this copy's: nothing is shown here.
        assertNull(repository.toShow.value)
    }

    @Test
    fun `an update that does not happen leaves the notes of the version that runs, also one version later`() {
        val next = release("0.2.0", 200).copy(notes = listOf("Channels"))
        val nextNotes = VersionNotes.of(next)
        val afterNext = release("0.3.0", 300).copy(notes = listOf("The update after"))

        // This copy keeps its version's notes as the catalogue names it, and they are seen.
        runBlocking { store.updateData { it.copy(seen = 90) } }
        val firstScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val first = WhatsNewRepository(catalogue, store, RUNNING, firstScope)
        fetched(thisRelease)
        saved("this version's notes") { it.notes == listOf(thisNotes) }
        first.markSeen()
        saved("this version seen") { it.seen == 100L }

        // The catalogue names the next version, whose update is started and does not happen.
        fetched(next)
        runBlocking { first.saveNotes(next) }
        assertEquals(listOf(thisNotes, nextNotes), runBlocking { store.data.first() }.notes)
        assertEquals(thisNotes, first.notes.await("this version's notes, with the catalogue naming the next") { true })
        catalogue.catalogue.value = CatalogueState()
        assertEquals(thisNotes, first.notes.await("this version's notes, with no catalogue") { true })

        // It is done later: this copy ends, and the new one shows the notes kept for it, with no catalogue.
        runBlocking { firstScope.coroutineContext.job.cancelAndJoin() }
        val newCopy = repository(RUNNING.copy(versionCode = 200, versionName = "0.2.0", lastUpdateTime = RUNNING.lastUpdateTime + 60_000))
        assertEquals(nextNotes, newCopy.toShow.await("the next version's notes") { it != null })
        newCopy.markSeen()
        saved("the next version seen") { it.seen == 200L }

        // Its own update, to the one after, is started and does not happen either.
        fetched(afterNext)
        runBlocking { newCopy.saveNotes(afterNext) }
        assertEquals(listOf(nextNotes, VersionNotes.of(afterNext)), runBlocking { store.data.first() }.notes)
        assertEquals(nextNotes, newCopy.notes.await("the next version's notes, with the catalogue naming the one after") { true })
        catalogue.catalogue.value = CatalogueState()
        assertEquals(nextNotes, newCopy.notes.await("the next version's notes, with no catalogue") { true })
        assertNull(newCopy.toShow.value)
    }
}

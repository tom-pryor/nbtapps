// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.InMemoryPrereleaseRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.update.RUNNING
import com.nbtapps.store.data.update.idleUpdater
import com.nbtapps.store.data.update.release
import com.nbtapps.store.data.whatsnew.VersionNotes
import com.nbtapps.store.data.whatsnew.WhatsNewRepository
import com.nbtapps.store.data.whatsnew.WhatsNewSettings
import com.nbtapps.store.data.whatsnew.WhatsNewStep
import com.nbtapps.store.data.whatsnew.whatsNewStore
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.PackageDetails
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The sheet of what is new over the catalogue, as its view model gives it: shown once, gone as soon as it is dismissed. */
class CatalogueWhatsNewTest {

    @get:Rule(order = 1)
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val storeScope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.IO))
    private val headUnits = InMemoryHeadUnitRepository()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val jobs by lazy {
        JobRepository(headUnits, mainDispatcherRule.track(FakeConnector()), identity, MemoryPairingKeys(), PackageStore(folder.newFolder("packages")), {}, scope)
    }

    /** RUNNING is 0.1.0, code 100, installed over an earlier copy; this catalogue names it, with its notes. */
    private val thisRelease = release("0.1.0", 100).copy(notes = listOf("Settings", "What's new"))
    private val thisNotes = VersionNotes.of(thisRelease)

    /** A catalogue that comes in at the first fetch, as the screen asks for one. */
    private class FetchedCatalogue(private val index: CatalogueIndex) : CatalogueRepository {
        override val catalogue = MutableStateFlow(CatalogueState())

        override suspend fun refresh() {
            catalogue.value = CatalogueState(index = index)
        }

        override suspend fun refreshIfStale() = Unit

        override suspend fun details(packageId: String): Result<PackageDetails> = Result.failure(UnsupportedOperationException())
    }

    private val catalogue = FetchedCatalogue(
        CatalogueIndex(schema = 1, source = CatalogueSourceInfo("test", "Test source"), serial = 1, app = thisRelease),
    )
    private val updater by lazy { idleUpdater(folder.newFolder("update"), scope) }

    private fun viewModel(toShow: Flow<VersionNotes?>, seen: () -> Unit) = mainDispatcherRule.track(
        CatalogueViewModel(catalogue, headUnits, jobs, updater, RUNNING, InMemoryPrereleaseRepository(), notesToShow = toShow, whatsNewSeen = seen),
    )

    private fun TestScope.collect(viewModel: CatalogueViewModel) {
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.whatsNew.collect {} }
    }

    /** The first value that [predicate] takes, which may be null: it is wrapped, so as not to be taken for the time running out. */
    private fun <T> Flow<T>.await(what: String, predicate: (T) -> Boolean): T = runBlocking {
        val found = withTimeoutOrNull(10_000) { listOf(first(predicate)) } ?: throw AssertionError("never came: $what")
        found.single()
    }

    @Test
    fun `the notes to show are the sheet's, and a dismissal takes them away at once and marks them seen`() = runTest {
        val toShow = MutableStateFlow<VersionNotes?>(null)
        var seen = 0
        val viewModel = viewModel(toShow) { seen++ }
        collect(viewModel)
        assertNull(viewModel.whatsNew.value)

        toShow.value = thisNotes
        assertEquals(thisNotes, viewModel.whatsNew.value)

        viewModel.dismissWhatsNew()
        // Gone before the store says they are seen, so that a rotation does not bring them back meanwhile.
        assertNull(viewModel.whatsNew.value)
        assertEquals(1, seen)
        toShow.value = thisNotes.copy(lines = listOf("Again"))
        assertNull(viewModel.whatsNew.value)
    }

    @Test
    fun `the sheet waits while another screen is on top or the menu is over the catalogue, and is not marked seen by that`() = runTest {
        var seen = 0
        val viewModel = viewModel(MutableStateFlow(thisNotes)) { seen++ }
        collect(viewModel)

        assertNull(notesOverCatalogue(viewModel.whatsNew.value, onTop = true, menuShown = true))
        assertNull(notesOverCatalogue(viewModel.whatsNew.value, onTop = false, menuShown = false))
        assertEquals(thisNotes, notesOverCatalogue(viewModel.whatsNew.value, onTop = true, menuShown = false))
        assertEquals(0, seen)
    }

    @Test
    fun `over the real store, the fetch brings the sheet, and once dismissed a later start shows none`() = runTest {
        val store = whatsNewStore(File(folder.root, "whats-new.json"), storeScope)
        store.updateData { it.copy(seen = 90) }
        val whatsNew = WhatsNewRepository(catalogue, store, RUNNING, scope)
        val first = viewModel(whatsNew.toShow, whatsNew::markSeen)
        collect(first)

        // The view model fetches the catalogue as it starts, and its release is the version that runs.
        assertEquals(thisNotes, first.whatsNew.await("the sheet") { it != null })
        first.dismissWhatsNew()
        assertNull(first.whatsNew.value)
        // The version seen and the notes kept are two writes, in either order: wait for both.
        val saved = WhatsNewSettings(seen = 100, notes = listOf(thisNotes))
        assertEquals(saved, store.data.await("the version seen, with its notes kept") { it == saved })

        // As the next start of the app finds it: the catalogue in memory, the store on the phone.
        val later = WhatsNewRepository(catalogue, store, RUNNING, scope)
        val second = viewModel(later.toShow, later::markSeen)
        collect(second)
        assertEquals(WhatsNewStep.Nothing, later.step.await("the later step") { it != null })
        assertNull(second.whatsNew.value)
    }
}

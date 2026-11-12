// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.update.APP_PACKAGE
import com.nbtapps.store.data.update.ApkFacts
import com.nbtapps.store.data.update.ApkSource
import com.nbtapps.store.data.update.AppUpdateOffer
import com.nbtapps.store.data.update.AppUpdater
import com.nbtapps.store.data.update.FakeApkInstaller
import com.nbtapps.store.data.update.RELEASE_CERT
import com.nbtapps.store.data.update.RUNNING
import com.nbtapps.store.data.update.UpdateState
import com.nbtapps.store.data.update.apkBytes
import com.nbtapps.store.data.update.release
import com.nbtapps.store.data.update.idleUpdater
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageVersion
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class CatalogueViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    // No head unit is saved and no link is opened: the list is the catalogue alone, as it was before it knew of any.
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val jobs by lazy {
        JobRepository(headUnits, FakeConnector(), identity, MemoryPairingKeys(), PackageStore(folder.newFolder("packages")), {}, scope)
    }

    @After
    fun tearDown() = scope.cancel()

    private class FakeCatalogueRepository(private val result: CatalogueState) : CatalogueRepository {
        override val catalogue = MutableStateFlow(CatalogueState())
        var refreshes = 0

        override suspend fun refresh() {
            refreshes++
            catalogue.value = result
        }

        override suspend fun details(packageId: String): Result<PackageDetails> =
            Result.failure(UnsupportedOperationException())
    }

    private fun pkg(id: String, name: String, category: String, summary: String = "", author: String = "NBTApp") =
        CataloguePackage(
            id = id, name = name, summary = summary, author = author, category = category,
            versions = listOf(
                PackageVersion("1.0.0", url = "https://example.invalid/$id", size = 1, sha256 = "00", manifestSha256 = "11"),
            ),
        )

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test source"),
        serial = 1,
        packages = listOf(
            pkg("org.example.zeta", "zeta tool", "utilities", summary = "Status daemon"),
            pkg("org.example.launcher", "App Launcher", "hmi", summary = "Adds a menu entry"),
            pkg("org.example.logs", "Log Viewer", "hmi", summary = "Browse logs", author = "Someone Else"),
        ),
    )

    private val repository = FakeCatalogueRepository(CatalogueState(index = index))
    // Lazy so the ViewModel is created after the rule has installed the test Main dispatcher.
    private val updater by lazy { idleUpdater(folder.newFolder("update"), scope) }
    private val viewModel by lazy { CatalogueViewModel(repository, headUnits, jobs, updater, RUNNING) }

    private fun TestScope.collectUiState() {
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
    }

    private val shownNames get() = viewModel.uiState.value.packages.map { it.name }

    @Test
    fun `loads the catalogue on start and lists packages by name`() = runTest {
        collectUiState()

        val state = viewModel.uiState.value
        assertTrue(state.loaded)
        assertEquals("Test source", state.sourceName)
        assertEquals(listOf("App Launcher", "Log Viewer", "zeta tool"), shownNames)
        assertEquals(listOf("hmi", "utilities"), state.categories)
        // With no head unit known there is nothing to mark the rows with, or to set apart.
        assertNull(state.headUnit)
        assertEquals(listOf(CatalogueSection.Kind.PLAIN), state.sections.map { it.kind })
        assertEquals(0, state.updateCount)
        assertEquals(1, repository.refreshes)
    }

    @Test
    fun `does not reload a catalogue that is already held`() = runTest {
        repository.catalogue.value = CatalogueState(index = index)
        collectUiState()

        assertEquals(0, repository.refreshes)
        assertTrue(viewModel.uiState.value.loaded)
    }

    @Test
    fun `a failed load is reported and not shown as loaded`() = runTest {
        val failing = FakeCatalogueRepository(CatalogueState(error = "no network"))
        val viewModel = CatalogueViewModel(failing, headUnits, jobs, updater, RUNNING)
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }

        assertFalse(viewModel.uiState.value.loaded)
        assertEquals("no network", viewModel.uiState.value.error)
    }

    @Test
    fun `search matches name, summary, author and id without regard to case`() = runTest {
        collectUiState()

        viewModel.setQuery("LAUNCH")
        assertEquals(listOf("App Launcher"), shownNames)

        viewModel.setQuery("daemon")
        assertEquals(listOf("zeta tool"), shownNames)

        viewModel.setQuery("someone")
        assertEquals(listOf("Log Viewer"), shownNames)

        viewModel.setQuery("org.example.logs")
        assertEquals(listOf("Log Viewer"), shownNames)

        viewModel.setQuery("nothing like this")
        assertTrue(shownNames.isEmpty())
    }

    @Test
    fun `every search word has to match`() = runTest {
        collectUiState()

        viewModel.setQuery("  log   someone ")
        assertEquals(listOf("Log Viewer"), shownNames)

        viewModel.setQuery("log launcher")
        assertTrue(shownNames.isEmpty())
    }

    @Test
    fun `category filter narrows the list and combines with search`() = runTest {
        collectUiState()

        viewModel.selectFilter(CatalogueFilter.Category("hmi"))
        assertEquals(listOf("App Launcher", "Log Viewer"), shownNames)

        viewModel.setQuery("log")
        assertEquals(listOf("Log Viewer"), shownNames)

        viewModel.selectFilter(CatalogueFilter.All)
        viewModel.setQuery("")
        assertEquals(3, shownNames.size)
    }

    @Test
    fun `a selected category that disappears on refresh stops filtering`() = runTest {
        collectUiState()
        viewModel.selectFilter(CatalogueFilter.Category("utilities"))

        repository.catalogue.value = CatalogueState(index = index.copy(packages = index.packages.drop(1)))

        assertEquals(CatalogueFilter.All, viewModel.uiState.value.filter)
        assertEquals(listOf("App Launcher", "Log Viewer"), shownNames)
    }

    @Test
    fun `a newer release of the app in the catalogue is offered, and updating downloads it and hands it to Android`() = runTest {
        val body = apkBytes("0.2.0")
        val newer = release("0.2.0", 200, body)
        val installer = FakeApkInstaller()
        val updater = AppUpdater(
            source = ApkSource(body),
            installed = RUNNING,
            dir = folder.newFolder("own-update"),
            inspector = { ApkFacts(APP_PACKAGE, 200, RELEASE_CERT) },
            installer = installer,
            busy = { false },
            scope = scope,
        )
        val offering = FakeCatalogueRepository(CatalogueState(index = index.copy(app = newer)))
        val viewModel = CatalogueViewModel(offering, headUnits, jobs, updater, RUNNING)
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }

        val card = requireNotNull(viewModel.uiState.value.appUpdate)
        assertEquals(AppUpdateOffer.Available(newer), card.offer)
        assertEquals("0.1.0", card.installedVersion)
        assertEquals(UpdateState.Idle, card.state)
        assertFalse(card.waitsForJob)

        viewModel.updateApp()
        // The updater works on its own scope, in real time.
        withContext(Dispatchers.Default) {
            withTimeout(10_000) {
                viewModel.uiState.first { it.appUpdate?.state == UpdateState.Installing }
                installer.count.first { it == 1 }
            }
        }
        assertEquals(body.toList(), installer.handed.single().toList())
    }

    @Test
    fun `with no newer release in the catalogue nothing is offered and updating does nothing`() = runTest {
        collectUiState()

        assertNull(viewModel.uiState.value.appUpdate)
        viewModel.updateApp()
        assertEquals(UpdateState.Idle, updater.state.value)
    }
}

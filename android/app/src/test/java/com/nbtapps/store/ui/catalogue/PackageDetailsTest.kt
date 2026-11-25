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
import com.nbtapps.store.data.catalogue.FileCatalogueSource
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageVersion
import java.io.IOException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * The details on a package's page, as the index behind it is fetched again: each test says in which order the
 * catalogue answers.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class PackageDetailsTest {

    @get:Rule(order = 1)
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    /** A catalogue that answers when it is told. */
    private class HeldCatalogue : CatalogueRepository {
        override val catalogue = MutableStateFlow(CatalogueState())

        /** The refreshes asked for, and the fetches of an old index. Each ends as the test completes it. */
        val refreshes = ArrayList<CompletableDeferred<Unit>>()
        val ifStale = ArrayList<CompletableDeferred<Unit>>()

        /** The loads of the details, in the order they were asked for. Each is answered as the test completes it. */
        val loads = ArrayList<CompletableDeferred<Result<PackageDetails>>>()

        override suspend fun refresh() = CompletableDeferred<Unit>().also { refreshes += it }.await()

        override suspend fun refreshIfStale() = CompletableDeferred<Unit>().also { ifStale += it }.await()

        override suspend fun details(packageId: String): Result<PackageDetails> =
            CompletableDeferred<Result<PackageDetails>>().also { loads += it }.await()
    }

    private val catalogue = HeldCatalogue()
    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val headUnits = InMemoryHeadUnitRepository()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy {
        JobRepository(headUnits, mainDispatcherRule.track(FakeConnector()), identity, MemoryPairingKeys(), store, {}, scope)
    }
    private val viewModel by lazy {
        val downloader = PackageDownloader(FileCatalogueSource(null, folder.root), store, scope)
        mainDispatcherRule.track(CataloguePackageViewModel(ID, catalogue, store, downloader, jobs, headUnits, InMemoryPrereleaseRepository()))
    }

    /** The catalogue holds an index of [serial] from now on, which offers [version]. */
    private fun TestScope.fetched(serial: Long, version: String = "1.0.0") {
        val versions = listOf(PackageVersion(version, url = "https://example.invalid/$version", size = 1, sha256 = version, manifestSha256 = "11"))
        catalogue.catalogue.value = CatalogueState(
            CatalogueIndex(1, CatalogueSourceInfo("test", "Test"), serial, packages = listOf(CataloguePackage(ID, "App", versions = versions))),
        )
        runCurrent()
    }

    private fun TestScope.open() {
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
        runCurrent()
    }

    private fun TestScope.answer(load: Int, description: String) {
        catalogue.loads[load].complete(Result.success(PackageDetails(ID, description)))
        runCurrent()
    }

    private fun TestScope.fail(load: Int, reason: String) {
        catalogue.loads[load].complete(Result.failure(IOException(reason)))
        runCurrent()
    }

    private val details: DetailsState get() = viewModel.uiState.value.details

    private fun loaded(description: String) = DetailsState.Loaded(PackageDetails(ID, description))

    @Test
    fun `with an index held the details are loaded at once, while an old index is fetched again`() = runTest {
        fetched(serial = 1)

        open()

        assertEquals(1, catalogue.ifStale.size)
        assertEquals(1, catalogue.loads.size)
        assertEquals(DetailsState.Loading, details)
        answer(0, "first")
        assertEquals(loaded("first"), details)
        assertFalse("the fetch is under way still", catalogue.ifStale.single().isCompleted)
        assertEquals(emptyList<Any>(), catalogue.refreshes)

        // A fetch that brings nothing new, or fails, loads nothing.
        catalogue.catalogue.value = catalogue.catalogue.value.copy(refreshing = true)
        catalogue.catalogue.value = catalogue.catalogue.value.copy(refreshing = false, error = "no network")
        fetched(serial = 1)
        catalogue.ifStale.single().complete(Unit)
        runCurrent()
        assertEquals(1, catalogue.loads.size)
        assertEquals(loaded("first"), details)
    }

    @Test
    fun `with no index held the details wait for it, and are loaded once`() = runTest {
        open()
        assertEquals(1, catalogue.refreshes.size)
        assertEquals(0, catalogue.loads.size)

        fetched(serial = 1)
        catalogue.refreshes.single().complete(Unit)
        runCurrent()

        assertEquals(1, catalogue.loads.size)
        assertEquals(0, catalogue.ifStale.size)
        answer(0, "first")
        assertEquals(loaded("first"), details)
    }

    @Test
    fun `with no index to be had the details fail, and are loaded once there is one`() = runTest {
        open()
        catalogue.catalogue.value = CatalogueState(error = "no network")
        catalogue.refreshes.single().complete(Unit)
        runCurrent()
        fail(0, "no details")
        assertEquals(DetailsState.Failed("no details"), details)

        fetched(serial = 1)

        assertEquals(2, catalogue.loads.size)
        assertEquals(DetailsState.Loading, details)
        answer(1, "first")
        assertEquals(loaded("first"), details)
    }

    @Test
    fun `a newer index brings its offer at once, and its details in place of the old ones once they are there`() = runTest {
        fetched(serial = 1, version = "1.0.0")
        open()
        answer(0, "first")

        fetched(serial = 2, version = "1.1.0")

        assertEquals("1.1.0", viewModel.uiState.value.get?.version?.version)
        assertEquals(2, catalogue.loads.size)
        assertEquals(loaded("first"), details)
        answer(1, "second")
        assertEquals(loaded("second"), details)

        // And again.
        fetched(serial = 3)
        assertEquals(3, catalogue.loads.size)
        answer(2, "third")
        assertEquals(loaded("third"), details)
    }

    @Test
    fun `the old details stay where the newer ones cannot be loaded`() = runTest {
        fetched(serial = 1)
        open()
        answer(0, "first")

        fetched(serial = 2)
        fail(1, "no network")

        assertEquals(loaded("first"), details)
        assertEquals("not tried again for the same index", 2, catalogue.loads.size)
    }

    // ---- loads that overlap ----

    /** The page with its first load under way, and a second begun for a newer index. */
    private fun TestScope.overlap() {
        fetched(serial = 1)
        open()
        fetched(serial = 2)
        assertEquals(2, catalogue.loads.size)
        assertEquals(DetailsState.Loading, details)
    }

    @Test
    fun `of two loads, the older answering last changes nothing`() = runTest {
        overlap()

        answer(1, "second")
        assertEquals(loaded("second"), details)
        answer(0, "first")

        assertEquals(loaded("second"), details)
    }

    @Test
    fun `of two loads, the older answering first is shown until the newer does`() = runTest {
        overlap()

        answer(0, "first")
        assertEquals(loaded("first"), details)
        answer(1, "second")

        assertEquals(loaded("second"), details)
    }

    @Test
    fun `of two loads, the older failing first leaves the page loading`() = runTest {
        overlap()

        fail(0, "no network")
        assertEquals(DetailsState.Loading, details)
        answer(1, "second")

        assertEquals(loaded("second"), details)
    }

    @Test
    fun `of two loads, the older failing last changes nothing`() = runTest {
        overlap()

        answer(1, "second")
        fail(0, "no network")

        assertEquals(loaded("second"), details)
    }

    @Test
    fun `of two loads, the newer failing first leaves it to the older to bring details`() = runTest {
        overlap()

        fail(1, "no network")
        assertEquals(DetailsState.Failed("no network"), details)
        answer(0, "first")

        assertEquals(loaded("first"), details)
    }

    @Test
    fun `of two loads that fail, the newer's reason is shown, whichever is last`() = runTest {
        overlap()
        fail(1, "newer")
        fail(0, "older")
        assertEquals(DetailsState.Failed("newer"), details)
    }

    @Test
    fun `of two loads that fail, the older first, the newer's reason is shown`() = runTest {
        overlap()
        fail(0, "older")
        fail(1, "newer")
        assertEquals(DetailsState.Failed("newer"), details)
    }

    /** The page with its first load failed and a Retry under way, and a third begun for a newer index. */
    private fun TestScope.retried() {
        fetched(serial = 1)
        open()
        fail(0, "no network")
        assertEquals(DetailsState.Failed("no network"), details)
        viewModel.retryDetails()
        runCurrent()
        assertEquals(DetailsState.Loading, details)
        fetched(serial = 2)
        assertEquals(3, catalogue.loads.size)
        assertEquals(DetailsState.Loading, details)
    }

    @Test
    fun `a Retry that answers after a newer load changes nothing`() = runTest {
        retried()

        answer(2, "second")
        answer(1, "first")

        assertEquals(loaded("second"), details)
    }

    @Test
    fun `a Retry that answers before a newer load is shown until that one is there`() = runTest {
        retried()

        answer(1, "first")
        assertEquals(loaded("first"), details)
        fail(2, "no network")

        assertEquals(loaded("first"), details)
    }

    @Test
    fun `a Retry while an older load is under way is the newer of the two`() = runTest {
        overlap()
        fail(1, "no network")
        viewModel.retryDetails()
        runCurrent()
        assertEquals(DetailsState.Loading, details)

        answer(2, "third")
        answer(0, "first")

        assertEquals(loaded("third"), details)
        assertTrue(catalogue.loads.all { it.isCompleted })
    }

    private companion object {
        const val ID = "org.example.app"
    }
}

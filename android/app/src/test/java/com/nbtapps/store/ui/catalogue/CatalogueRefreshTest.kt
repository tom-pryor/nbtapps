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
import com.nbtapps.store.data.jobs.Refusal
import com.nbtapps.store.data.update.RUNNING
import com.nbtapps.store.data.update.idleUpdater
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.TcpEndpoint
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The catalogue screen's view model over the real job repository and a stand-in agent: what a pull on the list reads again. */
class CatalogueRefreshTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private class CountingCatalogue(private val index: CatalogueIndex) : CatalogueRepository {
        override val catalogue = MutableStateFlow(CatalogueState())
        val refreshes = AtomicInteger()

        override suspend fun refresh() {
            refreshes.incrementAndGet()
            catalogue.value = CatalogueState(index = index)
        }

        override suspend fun details(packageId: String): Result<PackageDetails> = Result.failure(UnsupportedOperationException())
    }

    private fun version(version: String) =
        PackageVersion(version, url = "https://example.invalid/$version", size = 1, sha256 = "00", manifestSha256 = "11")

    private val catalogue = CountingCatalogue(
        CatalogueIndex(
            schema = 1,
            source = CatalogueSourceInfo("test", "Test source"),
            serial = 1,
            packages = listOf(
                CataloguePackage("org.nbtapp.runtime", "NBT Runtime", versions = listOf(version("2.1.0"), version("2.0.0"))),
                CataloguePackage("org.nbtapp.hello", "Hello NBT", versions = listOf(version("1.2.0"))),
            ),
        ),
    )

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val jobs by lazy {
        JobRepository(headUnits, connector, identity, MemoryPairingKeys(), PackageStore(folder.newFolder("packages")), {}, scope, now = { 1234L })
    }
    private val viewModel by lazy {
        CatalogueViewModel(catalogue, headUnits, jobs, idleUpdater(folder.newFolder("update"), scope), RUNNING)
    }

    private val car = HeadUnit("car", "Car", listOf(TcpEndpoint("192.168.0.1", 7144)))

    private fun row(version: String) =
        """[{"id":"org.nbtapp.runtime","name":"NBT Runtime","version":"$version","key":"${"ab".repeat(32)}","flags":[]}]"""

    @After
    fun tearDown() = scope.cancel()

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(car)
            val collecting = launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
            block()
            collecting.cancel()
        }
    }

    private suspend fun await(what: String, predicate: (CatalogueUiState) -> Boolean): CatalogueUiState =
        withTimeoutOrNull(10_000) { viewModel.uiState.first(predicate) }
            ?: throw AssertionError("never came: $what; last was ${viewModel.uiState.value}")

    private fun CatalogueUiState.runtime() = sections.flatMap { it.rows }.single { it.pkg.id == "org.nbtapp.runtime" }.status!!

    @Test
    fun `over a link, a pull fetches the catalogue and reads the head unit's list again`() = test {
        connector.onConnect = { it.installed = row("2.0.0") }
        assertTrue(jobs.connect("car").isSuccess)
        val before = await("the link's list") { it.loaded && it.headUnit?.live == true }
        assertEquals("2.0.0", before.runtime().installed)
        assertEquals(1, before.updateCount)
        assertEquals(1, catalogue.refreshes.get())

        // Something else updated it meanwhile: another phone, a USB stick. The agent does not say so by itself.
        connector.agent.installed = row("2.1.0")
        viewModel.refresh()

        val after = await("the list read again") { it.runtime().installed == "2.1.0" && !it.refreshing }
        assertEquals(0, after.updateCount)
        assertNull(after.headUnitError)
        assertFalse(after.refreshingHeadUnit)
        assertEquals(2, catalogue.refreshes.get())
        // Once for the connect, once for the pull.
        assertEquals(2, connector.agent.requestsOf("packages.list").size)
        // And what was read is what is kept for when there is no link.
        jobs.disconnect()
        val saved = await("the saved list") { it.headUnit?.live == false }
        assertEquals("2.1.0", saved.runtime().installed)
    }

    @Test
    fun `with no link, a pull fetches the catalogue alone`() = test {
        await("the catalogue") { it.loaded }

        viewModel.refresh()

        await("the second fetch") { catalogue.refreshes.get() == 2 && !it.refreshing }
        assertTrue(connector.agents.isEmpty())
        assertNull(viewModel.uiState.value.headUnitError)
    }

    @Test
    fun `a head unit that cannot be read again says so, and the catalogue is fetched all the same`() = test {
        connector.onConnect = { it.installed = row("2.0.0") }
        assertTrue(jobs.connect("car").isSuccess)
        await("the link's list") { it.loaded && it.headUnit?.live == true }

        connector.agent.handlers["info.get"] = { throw Refusal("internal") }
        viewModel.refresh()

        val failed = await("the failure") { it.headUnitError != null && !it.refreshing }
        assertEquals(2, catalogue.refreshes.get())
        // The list from before stands.
        assertEquals("2.0.0", failed.runtime().installed)
    }
}

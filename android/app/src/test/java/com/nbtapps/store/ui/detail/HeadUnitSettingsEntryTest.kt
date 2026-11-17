// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.FakePhoneBluetooth
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.InMemoryPrereleaseRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.DefaultCatalogueRepository
import com.nbtapps.store.data.catalogue.FileCatalogueSource
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.TcpEndpoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The Settings button of an installed package's row on the head unit's page: there with a link to an agent that has settings, for a package that has some. */
class HeadUnitSettingsEntryTest {

    @get:Rule(order = 1)
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = mainDispatcherRule.track(FakeConnector())
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Test phone"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy { JobRepository(headUnits, connector, identity, MemoryPairingKeys(), store, {}, scope) }
    private val setup by lazy {
        val source = FileCatalogueSource(null, AgentVectors.file("packages/good"))
        AutostartSetup(jobs, DefaultCatalogueRepository(source), store, PackageDownloader(source, store, scope), InMemoryPrereleaseRepository(), scope)
    }

    private val installed = """[{"id":"org.nbtapp.dial","name":"Dial","version":"1.0.0","settings":true},""" +
        """{"id":"org.nbtapp.runtime","name":"Runtime","version":"2.1.0","logs":true}]"""

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144))))
            block()
        }
    }

    private fun viewModel() =
        mainDispatcherRule.track(HeadUnitDetailViewModel("car", headUnits, jobs, connector, FakePhoneBluetooth(), setup, scope))

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    /** The ids of the rows that offer Settings. */
    private fun HeadUnitDetailUiState.offering() = installed.filter { showsSettings(this, it) }.map { it.id }

    @Test
    fun `a package with settings offers them over a link to an agent that has them, and only then`() = test {
        connector.onConnect = { agent ->
            agent.capabilities = """["files","settings"]"""
            agent.installed = installed
        }
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        val page = await(viewModel.uiState, "the link") { it.link is LinkState.Connected && it.installed.size == 2 }
        assertTrue(page.settings)
        assertEquals(listOf("org.nbtapp.dial"), page.offering())

        // Without the link the saved list still says which have settings, and none offers them.
        jobs.disconnect()
        val offline = await(viewModel.uiState, "the link gone") { it.link == LinkState.Idle }
        assertTrue(offline.installed.first { it.id == "org.nbtapp.dial" }.settings)
        assertFalse(offline.settings)
        assertEquals(emptyList<String>(), offline.offering())
    }

    @Test
    fun `an agent from before settings offers none`() = test {
        connector.onConnect = { agent ->
            agent.capabilities = """["files"]"""
            agent.installed = installed
        }
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        val page = await(viewModel.uiState, "the link") { it.link is LinkState.Connected && it.installed.size == 2 }
        assertFalse(page.settings)
        assertEquals(emptyList<String>(), page.offering())
    }
}

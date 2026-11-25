// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.BluetoothAccess
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
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.withTestTimeout
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The head unit's page while the phone's Bluetooth is switched off and on: its Bluetooth line, and why a link ended. */
class HeadUnitBluetoothTest {

    @get:Rule(order = 1)
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = mainDispatcherRule.track(FakeConnector(serial = true))
    private val bluetooth = FakePhoneBluetooth()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Test phone"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy {
        JobRepository(headUnits, connector, identity, MemoryPairingKeys(), store, {}, scope, now = { 1234L }, bluetoothOn = bluetooth.on)
    }
    private val setup by lazy {
        val source = FileCatalogueSource(null, AgentVectors.file("packages/good"))
        AutostartSetup(jobs, DefaultCatalogueRepository(source, scope), store, PackageDownloader(source, store, scope), InMemoryPrereleaseRepository(), scope)
    }

    private val car = HeadUnit("car", "Test car", listOf(SerialEndpoint("00:11:22:33:44:55", "Test car")))

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTestTimeout(60_000) {
            headUnits.add(car)
            block()
        }
    }

    private fun viewModel() = mainDispatcherRule.track(HeadUnitDetailViewModel("car", headUnits, jobs, connector, bluetooth, setup, scope))

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    /** The phone's Bluetooth is switched on or off, and the phone says so. */
    private fun switch(on: Boolean) {
        bluetooth.access = if (on) BluetoothAccess.ON else BluetoothAccess.OFF
        bluetooth.refreshConnected()
    }

    @Test
    fun `the Bluetooth line changes while the page is open, as Bluetooth is switched off and on`() = test {
        val viewModel = viewModel()
        assertEquals(SerialStatus(problem = null, needsPermission = false), await(viewModel.uiState, "the line") { it.serial != null }.serial)

        switch(on = false)
        assertEquals("Bluetooth is off", await(viewModel.uiState, "Bluetooth off") { it.serial?.problem != null }.serial?.problem)

        switch(on = true)
        await(viewModel.uiState, "Bluetooth on again") { it.serial != null && it.serial.problem == null }
    }

    @Test
    fun `a link that ends as Bluetooth is switched off says so, and the line says it is off`() = test {
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        await(viewModel.uiState, "the link") { it.link is LinkState.Connected }

        switch(on = false)
        connector.agent.drop()

        val page = await(viewModel.uiState, "the end") { it.link is LinkState.Failed && it.serial?.problem != null }
        assertEquals(LinkState.Failed("This phone's Bluetooth was switched off"), page.link)
        assertEquals("Bluetooth is off", page.serial?.problem)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.data.FakePhoneBluetooth
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.SerialEndpoint
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class HeadUnitsViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    /** A build's links, as far as the screen asks: which kinds of endpoint they reach. Nothing is opened. */
    private class FakeConnector(private val tcp: Boolean) : LinkConnector {
        override fun supports(endpoint: Endpoint) = endpoint is SerialEndpoint || tcp
        override suspend fun connect(endpoint: Endpoint): Link = error("not opened in these tests")
    }

    private val repository = InMemoryHeadUnitRepository()
    private val bluetooth = FakePhoneBluetooth()
    // Lazy so the ViewModel is created after the rule has installed the test Main dispatcher.
    private val keys = MemoryPairingKeys()
    private val viewModel by lazy { HeadUnitsViewModel(repository, bluetooth, FakeConnector(tcp = true), keys) }

    /** The release build's: Bluetooth serial only. */
    private val serialOnly by lazy { HeadUnitsViewModel(repository, bluetooth, FakeConnector(tcp = false), keys) }

    @Test
    fun `adding a valid head unit shows it in the list`() = runTest {
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }

        assertTrue(viewModel.addHeadUnit(HeadUnitForm("Car", "192.168.1.1", "22")))

        val units = viewModel.uiState.value.headUnits
        assertEquals(listOf("Car"), units.map { it.name })
    }

    @Test
    fun `invalid head unit is not added`() = runTest {
        assertFalse(viewModel.addHeadUnit(HeadUnitForm(name = "Car")))
        assertTrue(repository.headUnits.first().isEmpty())
    }

    @Test
    fun `a network address is asked for only where the build has a TCP link`() = runTest {
        assertTrue(viewModel.uiState.value.takesAddress)
        assertFalse(serialOnly.uiState.value.takesAddress)
    }

    @Test
    fun `without a TCP link a head unit with only an address is not added, and an address beside the car is dropped`() = runTest {
        assertFalse(serialOnly.addHeadUnit(HeadUnitForm("Car", "192.168.1.1", "22")))
        assertTrue(repository.headUnits.first().isEmpty())

        val car = bluetooth.devices.first()
        assertTrue(serialOnly.addHeadUnit(HeadUnitForm("Car", "192.168.1.1", "22").withDevice(car)))
        assertEquals(listOf(SerialEndpoint(car.address, car.name)), repository.headUnits.first().single().endpoints)
    }

    @Test
    fun `the paired devices are there to pick the car from`() = runTest {
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }

        val choice = viewModel.uiState.value.bluetooth
        assertEquals(BluetoothAccess.ON, choice.access)
        assertEquals(listOf("Test Car", "Headphones"), choice.devices.map { it.name })
    }

    @Test
    fun `Bluetooth is read again when asked, not before`() = runTest {
        bluetooth.access = BluetoothAccess.NOT_ALLOWED
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
        assertEquals(BluetoothChoice(BluetoothAccess.NOT_ALLOWED, emptyList()), viewModel.uiState.value.bluetooth)

        // The user grants the permission in the system's prompt.
        bluetooth.access = BluetoothAccess.ON
        assertEquals(BluetoothAccess.NOT_ALLOWED, viewModel.uiState.value.bluetooth.access)
        viewModel.refreshBluetooth()
        assertEquals(BluetoothChoice(BluetoothAccess.ON, bluetooth.devices), viewModel.uiState.value.bluetooth)

        // The user unpairs a device in the phone's settings and comes back.
        bluetooth.devices = bluetooth.devices.take(1)
        viewModel.refreshBluetooth()
        assertEquals(listOf("Test Car"), viewModel.uiState.value.bluetooth.devices.map { it.name })
    }

    @Test
    fun `a head unit added with the car picked is saved with its Bluetooth serial endpoint`() = runTest {
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
        val car = viewModel.uiState.value.bluetooth.devices.first()

        assertTrue(viewModel.addHeadUnit(HeadUnitForm().withDevice(car)))

        val unit = repository.headUnits.first().single()
        assertEquals("Test Car", unit.name)
        assertEquals(listOf(SerialEndpoint("00:11:22:33:44:55", "Test Car")), unit.endpoints)
    }

    @Test
    fun `removing a head unit drops it from the list`() = runTest {
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
        viewModel.addHeadUnit(HeadUnitForm("Car", "192.168.1.1", "22"))
        val id = viewModel.uiState.value.headUnits.single().id

        viewModel.removeHeadUnit(id)

        assertTrue(viewModel.uiState.value.headUnits.isEmpty())
    }

    @Test
    fun `removing a head unit drops the key it issued this phone`() = runTest {
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
        viewModel.addHeadUnit(HeadUnitForm("Car", "192.168.1.1", "22"))
        val id = viewModel.uiState.value.headUnits.single().id
        // The app has been connected to it and paired with it.
        val agentId = "000102030405060708090a0b0c0d0e0f"
        repository.update(id) { it.copy(headUnitId = agentId) }
        keys.put(agentId, ByteArray(32) { 7 })

        viewModel.removeHeadUnit(id)

        assertTrue(viewModel.uiState.value.headUnits.isEmpty())
        assertEquals(emptySet<String>(), keys.headUnitIds)
    }
}

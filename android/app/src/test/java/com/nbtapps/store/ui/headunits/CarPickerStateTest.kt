// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.data.FakePhoneBluetooth
import com.nbtapps.store.model.PairedDevice
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/** What the car picker shows, and which car it picks before the user does. */
class CarPickerStateTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    private val car = PairedDevice("BMW 12345", "AA:BB:CC:DD:EE:FF", car = true)
    private val van = PairedDevice("van", "AA:BB:CC:DD:EE:01", car = true)
    private val headphones = PairedDevice("Headphones", "66:77:88:99:AA:BB")
    private val watch = PairedDevice("A watch", "66:77:88:99:AA:01")

    @Test
    fun `each state that blocks the list is told apart, and the first in the way is the one shown`() {
        val devices = listOf(car)
        assertEquals(CarPickerState.NoBluetooth, carPickerState(BluetoothAccess.MISSING, devices, emptySet(), refusedForGood = true))
        assertEquals(CarPickerState.NeedsPermission, carPickerState(BluetoothAccess.NOT_ALLOWED, emptyList(), emptySet(), refusedForGood = false))
        assertEquals(CarPickerState.PermissionRefused, carPickerState(BluetoothAccess.NOT_ALLOWED, emptyList(), emptySet(), refusedForGood = true))
        assertEquals(CarPickerState.BluetoothOff, carPickerState(BluetoothAccess.OFF, emptyList(), emptySet(), refusedForGood = true))
        assertEquals(CarPickerState.NothingPaired, carPickerState(BluetoothAccess.ON, emptyList(), emptySet(), refusedForGood = false))
        // A refusal said before the permission was granted after all counts for nothing.
        assertTrue(carPickerState(BluetoothAccess.ON, devices, emptySet(), refusedForGood = true) is CarPickerState.Devices)
    }

    @Test
    fun `cars and other devices are listed apart, each with the connected ones first and then by name`() {
        val state = carPickerState(BluetoothAccess.ON, listOf(headphones, van, watch, car), setOf("aa:bb:cc:dd:ee:01", "66:77:88:99:AA:BB"), false)

        assertEquals(
            CarPickerState.Devices(
                cars = listOf(CarChoice(van, connected = true), CarChoice(car, connected = false)),
                others = listOf(CarChoice(headphones, connected = true), CarChoice(watch, connected = false)),
            ),
            state,
        )
        state as CarPickerState.Devices
        assertEquals(headphones, state.find("66:77:88:99:aa:bb")?.device)
        assertNull(state.find("00:00:00:00:00:01"))
        assertNull(state.find(null))
    }

    @Test
    fun `where no device says it is a car, every device is in the main list`() {
        val plain = car.copy(car = false)
        val state = carPickerState(BluetoothAccess.ON, listOf(headphones, plain), setOf(plain.address), false)

        assertEquals(CarPickerState.Devices(listOf(CarChoice(plain, true), CarChoice(headphones, false)), emptyList()), state)
    }

    @Test
    fun `the car picked to begin with is the one connected, else the only one, else none`() {
        // One of two cars is connected.
        assertEquals(van, defaultCar(listOf(car, van, headphones), setOf("aa:bb:cc:dd:ee:01", headphones.address)))
        // Both are, or neither is: the user picks.
        assertNull(defaultCar(listOf(car, van), setOf(car.address, van.address)))
        assertNull(defaultCar(listOf(car, van), emptySet()))
        assertNull(defaultCar(listOf(car, van, headphones), setOf(headphones.address)))
        // The only car, connected or not; other devices do not count, even a connected one.
        assertEquals(car, defaultCar(listOf(car, headphones), setOf(headphones.address)))
        assertEquals(car, defaultCar(listOf(car, headphones), emptySet()))
        assertNull(defaultCar(emptyList(), emptySet()))
    }

    @Test
    fun `where no device says it is a car, none is picked to begin with, though the list has them all`() {
        val plain = car.copy(car = false)
        // A lone paired device.
        assertNull(defaultCar(listOf(headphones), emptySet()))
        assertNull(defaultCar(listOf(plain), setOf(plain.address)))
        // One of two is connected.
        assertNull(defaultCar(listOf(plain, headphones), setOf(headphones.address)))
        assertNull(defaultCar(listOf(plain, headphones), emptySet()))
    }

    @Test
    fun `the other devices' row counts them`() {
        assertEquals("1 that doesn't look like a car", otherDevicesLine(1))
        assertEquals("3 that don't look like cars", otherDevicesLine(3))
    }

    @Test
    fun `each blocked state says why and offers the one button that helps, and the compact form says one line`() {
        assertNull(carPickerMessage(CarPickerState.Devices(listOf(CarChoice(car, false)), emptyList()), compact = false))

        val allow = carPickerMessage(CarPickerState.NeedsPermission, compact = false)!!
        assertEquals(CarPickerAction.ALLOW, allow.action)
        assertEquals(
            "Android asks for this before an app may list the phone's paired Bluetooth devices or connect to one. " +
                "NBTApps uses it only to find the car and reach it.",
            allow.detail,
        )
        assertEquals(CarPickerAction.APP_SETTINGS, carPickerMessage(CarPickerState.PermissionRefused, compact = false)?.action)
        assertEquals(CarPickerAction.SWITCH_ON, carPickerMessage(CarPickerState.BluetoothOff, compact = false)?.action)
        assertEquals(CarPickerAction.BLUETOOTH_SETTINGS, carPickerMessage(CarPickerState.NothingPaired, compact = false)?.action)
        // Nothing the app can do about a phone without Bluetooth.
        assertNull(carPickerMessage(CarPickerState.NoBluetooth, compact = false)?.action)

        for (state in listOf(CarPickerState.NeedsPermission, CarPickerState.PermissionRefused, CarPickerState.BluetoothOff, CarPickerState.NoBluetooth, CarPickerState.NothingPaired)) {
            val full = carPickerMessage(state, compact = false)!!
            val compact = carPickerMessage(state, compact = true)!!
            assertEquals(full.copy(detail = null), compact)
            assertTrue(state.toString(), '\n' !in compact.text)
        }
        // The line's link is a part of it.
        assertTrue(CAR_NOT_LISTED_LINK in CAR_NOT_LISTED)
    }

    @Test
    fun `the picker's view model reads the phone's Bluetooth again when asked, connections included`() {
        val phone = FakePhoneBluetooth(access = BluetoothAccess.NOT_ALLOWED, devices = listOf(car, headphones), links = setOf(car.address))
        val viewModel = CarPickerViewModel(phone)
        assertEquals(BluetoothChoice(BluetoothAccess.NOT_ALLOWED, emptyList()), viewModel.bluetooth.value)
        assertEquals(emptySet<String>(), viewModel.connected.value)

        // The user grants the permission.
        phone.access = BluetoothAccess.ON
        viewModel.refresh()
        assertEquals(BluetoothChoice(BluetoothAccess.ON, listOf(car, headphones)), viewModel.bluetooth.value)
        assertEquals(setOf(car.address), viewModel.connected.value)
    }
}

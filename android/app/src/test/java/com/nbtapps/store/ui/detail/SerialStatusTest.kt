// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.data.FakePhoneBluetooth
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** What the head unit's screen says of its Bluetooth serial endpoint before any link is opened. */
class SerialStatusTest {

    private val bluetooth = FakePhoneBluetooth()
    private val wifi = TcpEndpoint("192.168.0.1", 7144)
    private val car = HeadUnit("car", "Car", listOf(wifi, SerialEndpoint("00:11:22:33:44:55", "Test Car")))

    @Test
    fun `with Bluetooth on and the car paired nothing stands in the way`() {
        assertEquals(SerialStatus(problem = null, needsPermission = false), bluetooth.serialStatus(car))
    }

    @Test
    fun `what stands in the way is named, and only the missing permission is the user's to grant here`() {
        bluetooth.access = BluetoothAccess.OFF
        assertEquals(SerialStatus("Bluetooth is off", needsPermission = false), bluetooth.serialStatus(car))

        bluetooth.access = BluetoothAccess.NOT_ALLOWED
        val status = requireNotNull(bluetooth.serialStatus(car))
        assertTrue(status.needsPermission)
        assertTrue(status.problem.orEmpty().contains("Nearby devices"))

        bluetooth.access = BluetoothAccess.MISSING
        assertEquals(SerialStatus("This phone has no Bluetooth", needsPermission = false), bluetooth.serialStatus(car))

        // The user unpairs the car in the phone's settings.
        bluetooth.access = BluetoothAccess.ON
        bluetooth.devices = bluetooth.devices.drop(1)
        assertEquals(SerialStatus("This phone isn't paired with the car", needsPermission = false), bluetooth.serialStatus(car))
    }

    @Test
    fun `a head unit with no Bluetooth serial endpoint has nothing to say of Bluetooth, and nor has one that is gone`() {
        bluetooth.access = BluetoothAccess.OFF
        assertNull(bluetooth.serialStatus(car.copy(endpoints = listOf(wifi))))
        assertNull(bluetooth.serialStatus(null))
    }
}

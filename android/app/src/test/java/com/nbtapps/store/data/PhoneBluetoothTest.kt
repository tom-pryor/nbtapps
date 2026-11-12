// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** What the app says of a Bluetooth serial link before one is tried. */
class PhoneBluetoothTest {

    private val phone = FakePhoneBluetooth()
    private val car = "00:11:22:33:44:55"

    @Test
    fun `a link can be tried when Bluetooth is on and the car is paired`() {
        assertNull(phone.serialUnavailable(car))
    }

    @Test
    fun `the address is the same one however its letters are written`() {
        assertNull(phone.serialUnavailable("66:77:88:99:aa:bb"))
    }

    @Test
    fun `each thing in the way has its own words, and the first in the way is the one named`() {
        phone.access = BluetoothAccess.MISSING
        assertEquals("This phone has no Bluetooth", phone.serialUnavailable(car))
        phone.access = BluetoothAccess.NOT_ALLOWED
        assertEquals(
            "The Nearby devices permission isn't granted, so the app can't reach the car over Bluetooth",
            phone.serialUnavailable(car),
        )
        phone.access = BluetoothAccess.OFF
        assertEquals("Bluetooth is off", phone.serialUnavailable(car))
        // Not paired is only known, and only said, with Bluetooth on.
        assertEquals("Bluetooth is off", phone.serialUnavailable("00:00:00:00:00:01"))
    }

    @Test
    fun `a car the phone is not paired with is not dialled`() {
        assertEquals("This phone isn't paired with the car", phone.serialUnavailable("00:00:00:00:00:01"))
        phone.devices = emptyList()
        assertEquals("This phone isn't paired with the car", phone.serialUnavailable(car))
    }

    @Test
    fun `a saved address that is no Bluetooth address is said to be none`() {
        for (bad in listOf("", "car", "00:11:22:33:44", "00:11:22:33:44:55:66", "00-11-22-33-44-55", "0G:11:22:33:44:55", " $car")) {
            assertEquals(bad, "The Bluetooth address saved for this head unit isn't one", phone.serialUnavailable(bad))
        }
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import com.nbtapps.store.model.PairedDevice
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
    fun `the devices connected now are the paired ones among those linked, in upper case`() {
        val paired = listOf(PairedDevice("BMW 12345", "AA:BB:CC:DD:EE:FF"), PairedDevice("Headphones", "66:77:88:99:AA:BB"))

        assertEquals(setOf("AA:BB:CC:DD:EE:FF"), connectedAmong(paired, listOf("aa:bb:cc:dd:ee:ff")))
        // Linked by both profiles, it is there once.
        assertEquals(setOf("AA:BB:CC:DD:EE:FF"), connectedAmong(paired, listOf("AA:BB:CC:DD:EE:FF", "aa:bb:cc:dd:ee:ff")))
        // A device the phone is not paired with is not one of them.
        assertEquals(setOf("66:77:88:99:AA:BB"), connectedAmong(paired, listOf("66:77:88:99:AA:BB", "00:00:00:00:00:01")))
        assertEquals(emptySet<String>(), connectedAmong(emptyList(), listOf("AA:BB:CC:DD:EE:FF")))
    }

    @Test
    fun `what is connected is read when asked, and only with Bluetooth on and allowed`() {
        phone.links = setOf("00:11:22:33:44:55")
        assertEquals("nothing is read before it is asked for", emptySet<String>(), phone.connected.value)

        phone.refreshConnected()
        assertEquals(setOf("00:11:22:33:44:55"), phone.connected.value)

        phone.access = BluetoothAccess.NOT_ALLOWED
        phone.refreshConnected()
        assertEquals(emptySet<String>(), phone.connected.value)

        phone.access = BluetoothAccess.ON
        phone.links = setOf("00:11:22:33:44:55", "66:77:88:99:aa:bb")
        phone.refreshConnected()
        assertEquals(setOf("00:11:22:33:44:55", "66:77:88:99:AA:BB"), phone.connected.value)
    }

    @Test
    fun `a saved address that is no Bluetooth address is said to be none`() {
        for (bad in listOf("", "car", "00:11:22:33:44", "00:11:22:33:44:55:66", "00-11-22-33-44-55", "0G:11:22:33:44:55", " $car")) {
            assertEquals(bad, "The Bluetooth address saved for this head unit isn't one", phone.serialUnavailable(bad))
        }
    }
}

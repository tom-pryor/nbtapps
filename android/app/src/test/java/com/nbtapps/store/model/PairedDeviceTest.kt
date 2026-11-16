// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Which Bluetooth classes say a device is a car. The numbers are Android's `BluetoothClass` constants. */
class PairedDeviceTest {

    private val audioVideo = 0x0400

    @Test
    fun `a hands-free kit or a car's audio is a car`() {
        assertTrue(isCarClass(audioVideo, 0x0408))
        assertTrue(isCarClass(audioVideo, 0x0420))
    }

    @Test
    fun `other audio devices, and devices of other kinds, are not`() {
        // Headphones, a wearable headset, a loudspeaker.
        assertFalse(isCarClass(audioVideo, 0x0418))
        assertFalse(isCarClass(audioVideo, 0x0404))
        assertFalse(isCarClass(audioVideo, 0x0414))
        // A phone and a computer.
        assertFalse(isCarClass(0x0200, 0x020C))
        assertFalse(isCarClass(0x0100, 0x010C))
        // A class that does not hold together is not trusted.
        assertFalse(isCarClass(0x0200, 0x0420))
        // A device is no car until its class says so.
        assertFalse(PairedDevice("BMW 12345", "AA:BB:CC:DD:EE:FF").car)
    }
}

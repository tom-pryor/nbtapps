// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import com.nbtapps.store.model.PairedDevice

/** A phone's Bluetooth that is whatever a test sets it to. */
class FakePhoneBluetooth(
    @Volatile var access: BluetoothAccess = BluetoothAccess.ON,
    @Volatile var devices: List<PairedDevice> = listOf(PairedDevice("Test Car", "00:11:22:33:44:55"), PairedDevice("Headphones", "66:77:88:99:AA:BB")),
) : PhoneBluetooth {
    override fun access() = access

    /** As the real one: nothing is listed unless Bluetooth is on and allowed. */
    override fun pairedDevices() = if (access == BluetoothAccess.ON) devices else emptyList()
}

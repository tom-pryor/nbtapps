// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import com.nbtapps.store.model.PairedDevice
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** A phone's Bluetooth that is whatever a test sets it to. */
class FakePhoneBluetooth(
    @Volatile var access: BluetoothAccess = BluetoothAccess.ON,
    @Volatile var devices: List<PairedDevice> = listOf(PairedDevice("Test Car", "00:11:22:33:44:55"), PairedDevice("Headphones", "66:77:88:99:AA:BB")),
    /** The addresses that have a hands-free or audio connection to the phone, as the phone would find on a read. */
    @Volatile var links: Set<String> = emptySet(),
) : PhoneBluetooth {
    override fun access() = access

    /** As the real one: nothing is listed unless Bluetooth is on and allowed. */
    override fun pairedDevices() = if (access == BluetoothAccess.ON) devices else emptyList()

    private val connectedNow = MutableStateFlow<Set<String>>(emptySet())

    override val connected: StateFlow<Set<String>> = connectedNow.asStateFlow()

    private val onNow = MutableStateFlow(access == BluetoothAccess.ON)

    /** As the real one: what [access] was at the last [refreshConnected], which a test calls after changing it. */
    override val on: StateFlow<Boolean> = onNow.asStateFlow()

    /** How many times [connected] was read again. */
    @Volatile var refreshes = 0
        private set

    /** As the real one: empty unless Bluetooth is on and allowed, and only paired devices, in upper case. */
    override fun refreshConnected() {
        refreshes++
        onNow.value = access == BluetoothAccess.ON
        connectedNow.value = connectedAmong(pairedDevices(), links)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import com.nbtapps.store.model.PairedDevice
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton

/** Whether this app can use the phone's Bluetooth at the moment. */
enum class BluetoothAccess {
    /** The phone has no Bluetooth. */
    MISSING,

    /** The user has not granted the Nearby devices permission. */
    NOT_ALLOWED,
    OFF,
    ON,
}

/**
 * What the screens and the links ask of the phone's Bluetooth before a link is opened: nothing here opens one,
 * starts a pairing or looks for devices. All of it can change while the app is in the background, so a screen
 * asks again when it comes back.
 */
interface PhoneBluetooth {
    fun access(): BluetoothAccess

    /** The devices paired with this phone, by name. Empty unless [access] is [BluetoothAccess.ON]. */
    fun pairedDevices(): List<PairedDevice>
}

/**
 * Why no Bluetooth serial link to the device at [address] can be tried at the moment, in words for the user.
 * Null if one can: the phone is paired with it and Bluetooth is on. Whether the car is in range, and whether
 * anything answers there, only a connect shows.
 */
fun PhoneBluetooth.serialUnavailable(address: String): String? = when (access()) {
    BluetoothAccess.MISSING -> "This phone has no Bluetooth"
    BluetoothAccess.NOT_ALLOWED -> "The Nearby devices permission isn't granted, so the app can't reach the car over Bluetooth"
    BluetoothAccess.OFF -> "Bluetooth is off"
    BluetoothAccess.ON -> when {
        !BLUETOOTH_ADDRESS.matches(address) -> "The Bluetooth address saved for this head unit isn't one"
        // An open to a device that is not paired would start a pairing from here, which is the user's to start.
        pairedDevices().none { it.address.equals(address, ignoreCase = true) } -> "This phone isn't paired with the car"
        else -> null
    }
}

private val BLUETOOTH_ADDRESS = Regex("([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}")

/** The phone's own Bluetooth. */
@Singleton
class AndroidPhoneBluetooth @Inject constructor(
    @ApplicationContext private val context: Context,
) : PhoneBluetooth {

    private val adapter: BluetoothAdapter?
        get() = context.getSystemService(BluetoothManager::class.java)?.adapter

    override fun access(): BluetoothAccess {
        val adapter = adapter ?: return BluetoothAccess.MISSING
        if (context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            return BluetoothAccess.NOT_ALLOWED
        }
        return if (adapter.isEnabled) BluetoothAccess.ON else BluetoothAccess.OFF
    }

    @SuppressLint("MissingPermission") // checked by access()
    override fun pairedDevices(): List<PairedDevice> {
        if (access() != BluetoothAccess.ON) return emptyList()
        return try {
            adapter?.bondedDevices.orEmpty()
                .map { PairedDevice(name = it.name ?: it.address, address = it.address) }
                .sortedBy { it.name.lowercase() }
        } catch (e: SecurityException) {
            // The permission went between the check and the call.
            emptyList()
        }
    }
}

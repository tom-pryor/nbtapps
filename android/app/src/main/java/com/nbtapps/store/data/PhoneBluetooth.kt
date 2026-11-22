// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothA2dp
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothHeadset
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.isCarClass
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

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

    /**
     * The addresses of the paired devices that have a hands-free (HFP) or an audio (A2DP) connection to this phone
     * now, as the phone writes them, in upper case: a car whose Bluetooth is up and holds the phone is among them.
     * Empty unless [access] is [BluetoothAccess.ON]. Nothing here opens a connection.
     *
     * It is empty until [refreshConnected] is first called. From then on it follows those connections as they come
     * and go, while the app's process runs; a screen still calls [refreshConnected] each time it comes back, since a
     * change of permission or a missed event is seen only then.
     */
    val connected: StateFlow<Set<String>>

    /** Reads [connected] again now. The first call also begins following the connections. */
    fun refreshConnected()

    /**
     * Whether the device at [address] has a media audio (A2DP) connection to this phone at this moment, as the phone's
     * audio profile says: read now, each time, and apart from [connected], which also counts hands-free. Null where the
     * phone cannot tell: Bluetooth off or not allowed, the profile not yet at hand, or an address that is not one.
     * Nothing here opens a connection.
     */
    fun mediaAudio(address: String): Boolean? = null

    /**
     * Whether Bluetooth is on and the app may use it ([access] is [BluetoothAccess.ON]), as [refreshConnected] last
     * found: so from its first call it follows Bluetooth being switched on and off, while the app's process runs.
     */
    val on: StateFlow<Boolean>
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

/**
 * Of the addresses [linked] that have a connection to the phone, the ones of devices in [paired], in upper case:
 * what [PhoneBluetooth.connected] holds. The phone writes addresses in upper case; a device written otherwise is
 * still the same one.
 */
fun connectedAmong(paired: List<PairedDevice>, linked: Iterable<String>): Set<String> {
    val addresses = paired.map { it.address.uppercase() }.toSet()
    return linked.map { it.uppercase() }.filter { it in addresses }.toSet()
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
                .map { device ->
                    val type = device.bluetoothClass
                    PairedDevice(
                        name = device.name ?: device.address,
                        address = device.address,
                        car = type != null && isCarClass(type.majorDeviceClass, type.deviceClass),
                    )
                }
                .sortedBy { it.name.lowercase() }
        } catch (e: SecurityException) {
            // The permission went between the check and the call.
            emptyList()
        }
    }

    private val connectedNow = MutableStateFlow<Set<String>>(emptySet())

    override val connected: StateFlow<Set<String>> = connectedNow.asStateFlow()

    private val onNow = MutableStateFlow(access() == BluetoothAccess.ON)

    override val on: StateFlow<Boolean> = onNow.asStateFlow()

    private val lock = Any()

    /** The profiles' proxies the system has handed over, by profile. They are asked for only once needed. */
    private val proxies = mutableMapOf<Int, BluetoothProfile>()

    /** The profiles whose proxy has been asked for and not yet handed over. */
    private val asked = mutableSetOf<Int>()

    private var following = false

    private val proxyListener = object : BluetoothProfile.ServiceListener {
        override fun onServiceConnected(profile: Int, proxy: BluetoothProfile) {
            synchronized(lock) {
                proxies[profile] = proxy
                asked -= profile
            }
            refreshConnected()
        }

        // As when Bluetooth is switched off: the proxy is asked for again once it is back on.
        override fun onServiceDisconnected(profile: Int) {
            synchronized(lock) {
                proxies.remove(profile)
                asked -= profile
            }
            refreshConnected()
        }
    }

    /** A connection of either profile came or went, or Bluetooth was switched on or off. */
    private val changes = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) = refreshConnected()
    }

    override fun refreshConnected() {
        follow()
        onNow.value = access() == BluetoothAccess.ON
        if (!onNow.value) {
            connectedNow.value = emptySet()
            return
        }
        askForProxies()
        connectedNow.value = readConnected()
    }

    /**
     * Registers for the two profiles' connection changes and Bluetooth's own, once, for as long as the process runs.
     * The system sends them only to an app that has the Nearby devices permission, so they begin once it is granted.
     */
    private fun follow() {
        synchronized(lock) {
            if (following) return
            following = true
        }
        val filter = IntentFilter().apply {
            addAction(BluetoothHeadset.ACTION_CONNECTION_STATE_CHANGED)
            addAction(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED)
            addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
        }
        // Not exported: the system's broadcasts still come, and no other app's do.
        context.registerReceiver(changes, filter, Context.RECEIVER_NOT_EXPORTED)
    }

    /** Asks the system for the proxies not yet had nor asked for. They come later, to [proxyListener]. */
    private fun askForProxies() {
        val adapter = adapter ?: return
        for (profile in PROFILES) {
            synchronized(lock) {
                if (profile in proxies || profile in asked) continue
                asked += profile
            }
            if (!adapter.getProfileProxy(context, proxyListener, profile)) synchronized(lock) { asked -= profile }
        }
    }

    @SuppressLint("MissingPermission") // checked by access() in refreshConnected()
    private fun readConnected(): Set<String> {
        val held = synchronized(lock) { proxies.values.toList() }
        if (held.isEmpty()) return emptySet()
        return try {
            connectedAmong(pairedDevices(), held.flatMap { it.connectedDevices }.map { it.address })
        } catch (e: SecurityException) {
            // The permission went between the check and the call.
            emptySet()
        }
    }

    @SuppressLint("MissingPermission") // checked by access()
    override fun mediaAudio(address: String): Boolean? {
        if (access() != BluetoothAccess.ON) return null
        // Asked for here too, where nothing has read the connections yet; the answer is then known from the next call.
        askForProxies()
        val audio = synchronized(lock) { proxies[BluetoothProfile.A2DP] } ?: return null
        return try {
            val device = adapter?.getRemoteDevice(address.uppercase()) ?: return null
            audio.getConnectionState(device) == BluetoothProfile.STATE_CONNECTED
        } catch (e: SecurityException) {
            // The permission went between the check and the call.
            null
        } catch (e: IllegalArgumentException) {
            null
        }
    }

    private companion object {
        /** Hands-free and audio: the two a car holds a phone by while its Bluetooth is up. */
        val PROFILES = listOf(BluetoothProfile.HEADSET, BluetoothProfile.A2DP)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.model.PairedDevice

/** A device paired with this phone as the car picker lists it. [connected]: it has a hands-free or audio connection now. */
data class CarChoice(val device: PairedDevice, val connected: Boolean)

/** What the car picker shows: see [carPickerState]. */
sealed interface CarPickerState {
    /** The Nearby devices permission is not granted, and Android can still be asked for it. */
    data object NeedsPermission : CarPickerState

    /** The user refused the Nearby devices permission for good: Android will not ask again, so only its settings can grant it. */
    data object PermissionRefused : CarPickerState

    data object BluetoothOff : CarPickerState

    /** The phone has no Bluetooth. */
    data object NoBluetooth : CarPickerState

    /** Bluetooth is on and allowed, and the phone is paired with nothing. */
    data object NothingPaired : CarPickerState

    /**
     * The paired devices, as [cars] and [others]: each the connected ones first, then by name. Where no device says
     * it is a car ([PairedDevice.car]), all of them are in [cars] and [others] is empty: a car may not say so.
     */
    data class Devices(val cars: List<CarChoice>, val others: List<CarChoice>) : CarPickerState {
        /** The device at [address] in either list, however its letters are written. */
        fun find(address: String?): CarChoice? =
            address?.let { (cars + others).firstOrNull { choice -> choice.device.address.equals(it, ignoreCase = true) } }
    }
}

/**
 * What the car picker shows, where the phone's Bluetooth stands at [access], is paired with [devices] and has
 * [connected] a connection to the addresses in it ([PhoneBluetooth.connected][com.nbtapps.store.data.PhoneBluetooth.connected]).
 * [refusedForGood]: the last ask for the Nearby devices permission was refused, and Android said it will not ask again.
 */
fun carPickerState(
    access: BluetoothAccess,
    devices: List<PairedDevice>,
    connected: Set<String>,
    refusedForGood: Boolean,
): CarPickerState = when (access) {
    BluetoothAccess.MISSING -> CarPickerState.NoBluetooth
    BluetoothAccess.NOT_ALLOWED -> if (refusedForGood) CarPickerState.PermissionRefused else CarPickerState.NeedsPermission
    BluetoothAccess.OFF -> CarPickerState.BluetoothOff
    BluetoothAccess.ON -> if (devices.isEmpty()) {
        CarPickerState.NothingPaired
    } else {
        val choices = devices
            .map { CarChoice(it, connected.any { address -> address.equals(it.address, ignoreCase = true) }) }
            .sortedWith(compareBy<CarChoice> { !it.connected }.thenBy { it.device.name.lowercase() })
        val (cars, others) = choices.partition { it.device.car }
        if (cars.isEmpty()) CarPickerState.Devices(others, emptyList()) else CarPickerState.Devices(cars, others)
    }
}

/**
 * The car to pick before the user does, among [devices], with [connected] as in [carPickerState]: the connected car,
 * if exactly one car is connected; else the only car, if there is exactly one; else none. The cars are the devices
 * that say they are one, or, where none does, every device, as the picker lists them.
 */
fun defaultCar(devices: List<PairedDevice>, connected: Set<String>): PairedDevice? {
    val cars = devices.filter { it.car }.ifEmpty { devices }
    cars.filter { car -> connected.any { it.equals(car.address, ignoreCase = true) } }.singleOrNull()?.let { return it }
    return cars.singleOrNull()
}

/** The second line of the row that opens the other paired devices: "3 that don't look like cars". */
fun otherDevicesLine(count: Int): String = if (count == 1) "1 that doesn't look like a car" else "$count that don't look like cars"

/** What a button of the car picker does: see [carPickerMessage]. */
enum class CarPickerAction(val label: String) {
    /** Asks for the Nearby devices permission. */
    ALLOW("Allow"),

    /** Asks Android to switch Bluetooth on. */
    SWITCH_ON("Switch Bluetooth on"),

    /** Opens Android's Bluetooth settings, where devices are paired. */
    BLUETOOTH_SETTINGS("Open Bluetooth settings"),

    /** Opens the app's page in Android's settings, where its permissions are granted. */
    APP_SETTINGS("Open app settings"),
}

/**
 * What the picker says in a state that blocks it: [text], then in the full form [detail] under it, and the one
 * button, [action], where there is something to do.
 */
data class CarPickerMessage(val text: String, val detail: String? = null, val action: CarPickerAction? = null)

/** What the picker says in [state], or null where it lists the devices. [compact]: one line, and no detail. */
fun carPickerMessage(state: CarPickerState, compact: Boolean): CarPickerMessage? {
    val full = when (state) {
        is CarPickerState.Devices -> return null
        CarPickerState.NeedsPermission -> CarPickerMessage(
            "To find the car, the app needs the Nearby devices permission.",
            NEARBY_DEVICES_WHY,
            CarPickerAction.ALLOW,
        )
        CarPickerState.PermissionRefused -> CarPickerMessage(
            "The Nearby devices permission was refused, and Android won't ask for it again.",
            "Allow it on the app's page in Android's settings, under Permissions. $NEARBY_DEVICES_WHY",
            CarPickerAction.APP_SETTINGS,
        )
        CarPickerState.BluetoothOff -> CarPickerMessage("Bluetooth is off. Switch it on to find the car.", action = CarPickerAction.SWITCH_ON)
        CarPickerState.NoBluetooth -> CarPickerMessage("This phone has no Bluetooth, so it can't reach a car.")
        CarPickerState.NothingPaired -> CarPickerMessage(
            "This phone isn't paired with any Bluetooth device yet.",
            "Pair it with the car in Bluetooth settings, then come back here.",
            CarPickerAction.BLUETOOTH_SETTINGS,
        )
    }
    return if (compact) full.copy(detail = null) else full
}

/** Why the app asks for the Nearby devices permission. */
const val NEARBY_DEVICES_WHY =
    "Android asks for this before an app may list the phone's paired Bluetooth devices or connect to one. " +
        "NBTApps uses it only to find the car and reach it."

/** The line under the list, with [CAR_NOT_LISTED_LINK] as the link to Bluetooth settings. */
const val CAR_NOT_LISTED = "Car not listed? Pair it in Bluetooth settings, then come back here."

/** The part of [CAR_NOT_LISTED] that is the link. */
const val CAR_NOT_LISTED_LINK = "Bluetooth settings"

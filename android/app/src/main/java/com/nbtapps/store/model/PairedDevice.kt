// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

/** A device paired with this phone over Bluetooth, such as the car. */
data class PairedDevice(
    val name: String,
    /** Its Bluetooth address, as the phone writes it: six pairs of hex digits with colons between. */
    val address: String,
    /**
     * It says it is a car: its Bluetooth class is the one of a car kit or a car's audio ([isCarClass]). A device that
     * does not say so may still be one.
     */
    val car: Boolean = false,
)

/** The major class of audio and video devices, in a device's Bluetooth class: `BluetoothClass.Device.Major.AUDIO_VIDEO`. */
private const val MAJOR_AUDIO_VIDEO = 0x0400

/** A hands-free kit, as a car's is: `BluetoothClass.Device.AUDIO_VIDEO_HANDSFREE`. */
private const val AUDIO_VIDEO_HANDSFREE = 0x0408

/** A car's audio: `BluetoothClass.Device.AUDIO_VIDEO_CAR_AUDIO`. */
private const val AUDIO_VIDEO_CAR_AUDIO = 0x0420

/**
 * Whether a device whose Bluetooth class has [majorDeviceClass] and [deviceClass] (as `BluetoothClass` gives them:
 * `getMajorDeviceClass` and `getDeviceClass`, the latter with the major bits in it) says it is a car: an audio and
 * video device that is a hands-free kit or a car's audio.
 */
fun isCarClass(majorDeviceClass: Int, deviceClass: Int): Boolean =
    majorDeviceClass == MAJOR_AUDIO_VIDEO && (deviceClass == AUDIO_VIDEO_HANDSFREE || deviceClass == AUDIO_VIDEO_CAR_AUDIO)

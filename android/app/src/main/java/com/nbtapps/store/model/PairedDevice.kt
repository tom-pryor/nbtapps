// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

/** A device paired with this phone over Bluetooth, such as the car. */
data class PairedDevice(
    val name: String,
    /** Its Bluetooth address, as the phone writes it: six pairs of hex digits with colons between. */
    val address: String,
)

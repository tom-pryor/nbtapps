// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint

/**
 * Raw user input for adding a head unit, with field validation. A head unit is reached over Bluetooth serial,
 * at a network address, or either: the form takes the car as paired with this phone, one TCP address, or both,
 * and wants at least one of the two. Where the build has no TCP link ([takesAddress] false) it takes the car only,
 * and any address in it is ignored.
 */
data class HeadUnitForm(
    val name: String = "",
    val host: String = "",
    val port: String = "",
    /** The car among the devices paired with this phone, for a Bluetooth serial link. Null for none. */
    val bluetooth: PairedDevice? = null,
    /** Whether this build can reach a network address at all, so whether the form asks for one. */
    val takesAddress: Boolean = true,
) {
    /** Whether a network address was begun: then both of its fields must be right. */
    private val hasAddress: Boolean
        get() = takesAddress && (host.isNotBlank() || port.isNotBlank())

    val nameError: String?
        get() = if (name.isBlank()) "Name is required" else null

    /** Set when the form names no way at all to reach the head unit. */
    val linkError: String?
        get() = when {
            bluetooth != null || hasAddress -> null
            takesAddress -> "Pick the car under Bluetooth, or enter a network address"
            else -> "Pick the car under Bluetooth"
        }

    val hostError: String?
        get() = when {
            !hasAddress -> null
            host.isBlank() -> "Host is required"
            host.trim().any(Char::isWhitespace) -> "Host can't contain spaces"
            else -> null
        }

    val portError: String?
        get() = if (!hasAddress || port.trim().toIntOrNull() in 1..65535) null else "Port must be 1–65535"

    val isValid: Boolean
        get() = nameError == null && linkError == null && hostError == null && portError == null

    fun toHeadUnit(id: String): HeadUnit {
        require(isValid) { "Form is invalid" }
        return HeadUnit(
            id = id,
            name = name.trim(),
            endpoints = listOfNotNull(
                if (hasAddress) TcpEndpoint(host.trim(), port.trim().toInt()) else null,
                bluetooth?.let { SerialEndpoint(address = it.address, deviceName = it.name) },
            ),
        )
    }
}

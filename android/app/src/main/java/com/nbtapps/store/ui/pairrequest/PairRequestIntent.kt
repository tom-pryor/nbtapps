// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import android.content.Intent

/**
 * The pairing code this intent hands over, or null if it is not such a request. Any app may send one, and its
 * extras are read only here, inside the catch: a bundle that cannot be unparcelled, as one with a class this app
 * does not have, throws on its first read, and is no request.
 */
fun Intent.pairRequest(): PairRequest? {
    if (action != PairRequest.ACTION) return null
    return try {
        val extras = extras ?: return null
        @Suppress("DEPRECATION")
        pairRequestOf(extras.get(PairRequest.EXTRA_CODE), extras.get(PairRequest.EXTRA_EXPIRES_IN_MS))
    } catch (e: Exception) {
        null
    }
}

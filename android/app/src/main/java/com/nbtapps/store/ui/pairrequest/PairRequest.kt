// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import com.nbtapps.store.data.link.ProtocolConstants

/**
 * A pairing code that another app on the phone handed to this one, so the user does not have to type it: see
 * docs/pairing-code-handover.md. [code] is the agent's code; [expiresInMs] is how long it was still good for when
 * it was handed over, or null if the other app did not say.
 */
data class PairRequest(val code: String, val expiresInMs: Long?) {
    companion object {
        const val ACTION = "com.nbtapps.store.action.PAIR_HEAD_UNIT"
        const val EXTRA_CODE = "com.nbtapps.store.extra.PAIRING_CODE"
        const val EXTRA_EXPIRES_IN_MS = "com.nbtapps.store.extra.EXPIRES_IN_MS"

        /** The id of the agent that issued the code. Reserved: not read yet. */
        const val EXTRA_AGENT_ID = "com.nbtapps.store.extra.AGENT_ID"

        /** The longest a code can still be good for: the agent's pairing window. */
        const val MAX_EXPIRES_IN_MS = ProtocolConstants.PAIRING_WINDOW_SECONDS * 1000L
    }
}

/**
 * The request in the extras' values [code] and [expiresInMs] as they came, or null if they are not one. The code
 * must be a String of exactly as many ASCII digits as the agent's codes have: a number would lose leading zeros.
 * The expiry, if there is one, must be a Long; it is kept within the agent's pairing window.
 */
internal fun pairRequestOf(code: Any?, expiresInMs: Any?): PairRequest? {
    if (code !is String || code.length != ProtocolConstants.PAIRING_CODE_DIGITS || !code.all { it in '0'..'9' }) return null
    val expires = when (expiresInMs) {
        null -> null
        is Long -> expiresInMs.coerceIn(0, PairRequest.MAX_EXPIRES_IN_MS)
        else -> return null
    }
    return PairRequest(code, expires)
}

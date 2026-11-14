// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PairRequestTest {

    @Test
    fun `six ASCII digits are a code, leading zeros and all`() {
        assertEquals(PairRequest("004217", null), pairRequestOf("004217", null))
        assertEquals(PairRequest("999999", null), pairRequestOf("999999", null))
    }

    @Test
    fun `anything else is no request`() {
        assertNull(pairRequestOf(null, null))
        assertNull(pairRequestOf("", null))
        assertNull(pairRequestOf("12345", null))
        assertNull(pairRequestOf("1234567", null))
        assertNull(pairRequestOf("12 456", null))
        assertNull(pairRequestOf("12345a", null))
        assertNull(pairRequestOf("-12345", null))
        // Digits of other scripts are digits to Char.isDigit, and not to the agent.
        assertNull(pairRequestOf("١٢٣٤٥٦", null))
        assertNull(pairRequestOf("１２３４５６", null))
        // A number has lost its leading zeros, if it had any.
        assertNull(pairRequestOf(123456, null))
        assertNull(pairRequestOf(123456L, null))
    }

    @Test
    fun `the expiry is kept within the pairing window, and without one there is none`() {
        assertEquals(90_000L, pairRequestOf("123456", 90_000L)?.expiresInMs)
        assertEquals(PairRequest.MAX_EXPIRES_IN_MS, pairRequestOf("123456", 3_600_000L)?.expiresInMs)
        assertEquals(PairRequest.MAX_EXPIRES_IN_MS, pairRequestOf("123456", Long.MAX_VALUE)?.expiresInMs)
        assertEquals(0L, pairRequestOf("123456", -5L)?.expiresInMs)
        assertEquals(0L, pairRequestOf("123456", Long.MIN_VALUE)?.expiresInMs)
        assertEquals(120_000L, PairRequest.MAX_EXPIRES_IN_MS)
        assertNull(pairRequestOf("123456", null)!!.expiresInMs)
    }

    @Test
    fun `an expiry that is not a Long makes it no request`() {
        assertNull(pairRequestOf("123456", 90_000))
        assertNull(pairRequestOf("123456", "90000"))
        assertNull(pairRequestOf("123456", 90.0))
    }
}

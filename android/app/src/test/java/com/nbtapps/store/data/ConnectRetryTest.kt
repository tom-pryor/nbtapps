// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import java.io.IOException
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.currentTime
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Assert.fail
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ConnectRetryTest {

    @Test
    fun `a first try that works is the only one`() = runTest {
        var tries = 0
        val result = connectWithRetry(20_000, 700, { currentTime }) { tries++; "link" }
        assertEquals("link", result)
        assertEquals(1, tries)
        assertEquals(0, currentTime)
    }

    @Test
    fun `tries again after a pause until one works`() = runTest {
        val seen = mutableListOf<Int>()
        val result = connectWithRetry(20_000, 700, { currentTime }) { attempt ->
            seen += attempt
            if (attempt < 4) throw IOException("busy")
            "link $attempt"
        }
        assertEquals("link 4", result)
        assertEquals(listOf(1, 2, 3, 4), seen)
        assertEquals(3 * 700L, currentTime)
    }

    @Test
    fun `gives up when no further try fits in the window, and says how hard it tried`() = runTest {
        var tries = 0
        val last = IOException("read failed")
        try {
            connectWithRetry<String>(2_000, 700, { currentTime }) { tries++; throw last }
            fail("expected a failure")
        } catch (e: ConnectFailedException) {
            // Tries at 0, 700 and 1400 ms; one more would start at 2100 ms, past the window.
            assertEquals(3, tries)
            assertEquals("Couldn't connect (3 tries over 1 s): read failed", e.message)
            assertSame(last, e.cause)
            assertEquals(3, e.attempts)
            assertEquals(1L, e.seconds)
            assertEquals("3 tries over 1 s", e.tries)
        }
    }

    @Test
    fun `a window too short for a second try makes one try`() = runTest {
        try {
            connectWithRetry<String>(500, 700, { currentTime }) { throw IOException("refused") }
            fail("expected a failure")
        } catch (e: ConnectFailedException) {
            assertEquals("Couldn't connect (1 try): refused", e.message)
            assertEquals("1 try", e.tries)
        }
    }

    @Test
    fun `a failure that is not about the link is not retried`() = runTest {
        var tries = 0
        try {
            connectWithRetry<String>(20_000, 700, { currentTime }) { tries++; throw SecurityException("no permission") }
            fail("expected a failure")
        } catch (e: SecurityException) {
            assertEquals(1, tries)
        }
    }
}

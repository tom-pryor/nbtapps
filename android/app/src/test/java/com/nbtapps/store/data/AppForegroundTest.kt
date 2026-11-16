// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** An [AppForeground] whose answer a test sets by hand. */
class FakeForeground(shown: Boolean = true) : AppForeground {
    val state = MutableStateFlow(shown)
    override val inForeground: StateFlow<Boolean> = state
}

/** Whether the app is in front of the user, from its activities' starts and stops, on the test's own clock. */
@OptIn(ExperimentalCoroutinesApi::class)
class AppForegroundTest {

    @Test
    fun `an activity that starts brings the app in at once, and the last that stops takes it out after the pause`() = runTest {
        val foreground = ActivityForeground(backgroundScope, leaveAfterMs = 700)
        assertFalse(foreground.inForeground.value)

        foreground.started()
        assertTrue(foreground.inForeground.value)

        foreground.stopped()
        advanceTimeBy(699)
        assertTrue("left before the pause was over", foreground.inForeground.value)
        advanceTimeBy(2)
        assertFalse(foreground.inForeground.value)
    }

    @Test
    fun `a rotation, which stops the activity and starts it again, is no leave`() = runTest {
        val foreground = ActivityForeground(backgroundScope, leaveAfterMs = 700)
        val seen = ArrayList<Boolean>()
        backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) { foreground.inForeground.collect { seen += it } }
        foreground.started()
        runCurrent()

        foreground.stopped()
        advanceTimeBy(50)
        foreground.started()
        advanceTimeBy(5_000)

        assertTrue(foreground.inForeground.value)
        // Started once, and never reported out of sight.
        assertTrue(seen.toString(), seen == listOf(false, true))
    }

    @Test
    fun `moving between two of the app's activities is no leave, and stopping both is`() = runTest {
        val foreground = ActivityForeground(backgroundScope, leaveAfterMs = 700)
        foreground.started()
        // The next activity starts before the one it covers stops.
        foreground.started()
        foreground.stopped()
        advanceTimeBy(5_000)
        assertTrue(foreground.inForeground.value)

        foreground.stopped()
        advanceTimeBy(701)
        assertFalse(foreground.inForeground.value)
    }
}

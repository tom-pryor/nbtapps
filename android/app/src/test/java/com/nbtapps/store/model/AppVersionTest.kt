// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** This app's own version, as a package's `requires.app` is held against it. */
class AppVersionTest {

    @After
    fun forget() {
        AppVersion.current = null
    }

    @Test
    fun `a trial build counts as the version it is a trial of`() {
        assertEquals("0.8.0", AppVersion.of("0.8.0"))
        assertEquals("0.8.0", AppVersion.of("0.8.0-trial"))
        assertEquals("0.8.0-rc.1", AppVersion.of("0.8.0-rc.1"))
        assertNull(AppVersion.of(""))
        assertNull(AppVersion.of("0.8"))

        AppVersion.current = AppVersion.of("0.8.0-trial")
        assertTrue(AppVersion.takes(">=0.8.0"))
    }

    @Test
    fun `a range takes in the versions it names, and no range or no known version takes every one`() {
        assertTrue(AppVersion.takes(">=9.0.0"))
        AppVersion.current = "0.7.0"
        assertTrue(AppVersion.takes(null))
        assertTrue(AppVersion.takes(">=0.7.0 <1.0.0"))
        assertFalse(AppVersion.takes(">=0.8.0"))
        assertFalse(AppVersion.takes("<0.7.0"))
        // One that is no range takes nothing, as for requires.agent.
        assertFalse(AppVersion.takes(">=0.7"))
    }
}

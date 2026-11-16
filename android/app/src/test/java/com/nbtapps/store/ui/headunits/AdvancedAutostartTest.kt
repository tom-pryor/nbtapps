// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import org.junit.Assert.assertEquals
import org.junit.Test

/** What the Advanced row of the pairing screens says of the autostart box behind it. */
class AdvancedAutostartTest {

    @Test
    fun `the closed row says how the box stands`() {
        assertEquals("Advanced", ADVANCED)
        assertEquals("Autostart is on", advancedAutostartLine(checked = true))
        assertEquals("Autostart is off", advancedAutostartLine(checked = false))
    }

    @Test
    fun `the warning says what clearing the box means`() {
        assertEquals("The agent won't start by itself", AUTOSTART_OFF_TITLE)
        assertEquals(
            "With autostart off, you start the agent by hand on the car every time you want to use the app, after each start of the car.",
            AUTOSTART_OFF_TEXT,
        )
    }
}

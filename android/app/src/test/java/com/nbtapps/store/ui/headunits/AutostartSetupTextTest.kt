// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.data.jobs.AutostartStep
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** What the card of the set-up that has the agent start with the car says at each step. */
class AutostartSetupTextTest {

    @Test
    fun `while it runs the card says what it is doing`() {
        assertEquals(AutostartSetupText("Checking the head unit…", working = true), autostartSetupText(AutostartStep.Checking, restartNeeded = false))
        assertEquals(AutostartSetupText("Getting the iDrive plugin…", working = true), autostartSetupText(AutostartStep.Downloading, restartNeeded = false))
        assertEquals(AutostartSetupText("Installing the iDrive plugin…", working = true), autostartSetupText(AutostartStep.Installing(), restartNeeded = false))
        assertEquals(AutostartSetupText("Installing the iDrive plugin…", working = true), autostartSetupText(AutostartStep.Installing("2021222324252627"), restartNeeded = true))
        assertEquals(AutostartSetupText("Switching on…", working = true), autostartSetupText(AutostartStep.SwitchingOn, restartNeeded = false))
    }

    @Test
    fun `once done it says whether anything changed, and offers the restart only where iDrive needs one`() {
        assertEquals(AutostartSetupText("The agent already starts with the car."), autostartSetupText(AutostartStep.Done(changed = false), restartNeeded = false))
        assertEquals(AutostartSetupText("The agent now starts with the car."), autostartSetupText(AutostartStep.Done(changed = true), restartNeeded = false))

        val restart = autostartSetupText(AutostartStep.Done(changed = true), restartNeeded = true)
        assertEquals("The agent now starts with the car.", restart.status)
        assertEquals(
            "iDrive has to restart before it loads the plugin. That restarts the whole head unit, and the app loses its link.",
            restart.restart,
        )
        // Asked for, it is not offered again.
        assertEquals(
            AutostartSetupText("iDrive is restarting. The agent starts about 15 seconds after it is back. Connect again then."),
            autostartSetupText(AutostartStep.Restarting, restartNeeded = true),
        )
    }

    @Test
    fun `a set-up that stopped short gives its reason and offers Retry, and no restart`() {
        val failed = autostartSetupText(AutostartStep.Failed("The head unit is busy with another job"), restartNeeded = true)

        assertEquals("The head unit is busy with another job", failed.status)
        assertTrue(failed.failed)
        assertNull(failed.restart)
    }

    @Test
    fun `the box says what ticking it does`() {
        assertEquals("Enable autostart", ENABLE_AUTOSTART)
        assertEquals(
            "The agent then starts with the car. The iDrive plugin does the starting, and is installed if the car doesn't have it.",
            ENABLE_AUTOSTART_NOTE,
        )
    }
}

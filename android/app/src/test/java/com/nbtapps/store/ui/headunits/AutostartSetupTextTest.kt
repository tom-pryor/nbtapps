// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.model.CheckMove
import com.nbtapps.store.model.CheckProblem
import com.nbtapps.store.model.CheckProblemFix
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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
            "The agent then starts with the car. The iDrive plugin does the starting, and is installed if the car doesn't have it. " +
                "Last, if an app was installed on the head unit by hand, you are asked about it.",
            ENABLE_AUTOSTART_NOTE,
        )
    }

    private val demo = CheckProblem(
        id = "aa-fix",
        title = "An app that was installed by hand",
        detail = "It stands in the way.",
        paths = listOf("/opt/demo/bin/demod", "/var/opt/demo/state"),
        moves = listOf(CheckMove("/plugins/Demo", "/opt/demo/Demo.old")),
        fix = CheckProblemFix("Remove that app", "Moves its plugin aside and removes the rest.", "hmi-restart"),
    )

    @Test
    fun `the question comes once the agent starts with the car, with the check's title, what its fix does, and its own button`() {
        val asked = autostartSetupText(AutostartStep.AskRemoval(demo, switchedOn = true), restartNeeded = true)
        assertEquals("The agent now starts with the car.", asked.lead)
        assertEquals("An app that was installed by hand", asked.status)
        assertEquals(
            "Last, the checks found this on the head unit. Its fix moves 1 folder aside, where it is kept, and removes 2 paths. " +
                "Each one is shown to you before anything changes; Skip leaves it all as it is, and the head unit's page keeps it.",
            asked.question,
        )
        assertEquals("Remove that app", asked.fixLabel)
        assertFalse(asked.working || asked.failed)
        // The one restart is offered only once the last question is answered.
        assertNull(asked.restart)

        val changed = autostartSetupText(AutostartStep.AskRemoval(demo.copy(paths = emptyList()), changed = true), restartNeeded = false)
        assertEquals("The agent already starts with the car.", changed.lead)
        assertEquals(
            "The files changed since they were listed. Last, the checks found this on the head unit. Its fix moves 1 folder " +
                "aside, where it is kept. Each one is shown to you before anything changes; Skip leaves it all as it is, and the " +
                "head unit's page keeps it.",
            changed.question,
        )
        assertEquals(
            "Last, the checks found this on the head unit. Its fix removes 1 path. Each one is shown to you before anything " +
                "changes; Skip leaves it all as it is, and the head unit's page keeps it.",
            removalQuestion(0, 1, changed = false),
        )

        val removing = autostartSetupText(AutostartStep.Removing(demo, switchedOn = true), restartNeeded = false)
        assertEquals("Putting this right: An app that was installed by hand…", removing.status)
        assertTrue(removing.working)
        assertEquals("The agent now starts with the car.", removing.lead)
    }

    @Test
    fun `once done it says what was moved aside and removed, and what was left or could not be done`() {
        val both = autostartSetupText(AutostartStep.Done(changed = true, moved = 1, removed = 3), restartNeeded = true)
        assertEquals("The agent now starts with the car. Of what was installed by hand, 1 folder was moved aside and 3 paths were removed.", both.status)
        assertEquals(AUTOSTART_RESTART_NEEDED, both.restart)
        assertNull(both.note)
        assertEquals(
            "The agent already starts with the car. Of what was installed by hand, 2 folders were moved aside.",
            autostartSetupText(AutostartStep.Done(changed = false, moved = 2), restartNeeded = false).status,
        )
        assertEquals(
            "The agent already starts with the car. Of what was installed by hand, 1 path was removed.",
            autostartSetupText(AutostartStep.Done(changed = false, removed = 1), restartNeeded = false).status,
        )
        // Where only the fix needs it, the restart is not said to be for the plugin.
        assertEquals(AUTOSTART_RESTART_TO_FINISH, autostartSetupText(AutostartStep.Done(changed = false, removed = 1), restartNeeded = true).restart)

        val left = autostartSetupText(AutostartStep.Done(changed = true, removed = 2, left = true), restartNeeded = true)
        assertEquals("Something is still there: the head unit's page shows what.", left.note)
        assertFalse(left.failed)
        val failed = autostartSetupText(AutostartStep.Done(changed = true, fixFailed = "The head unit couldn't remove everything."), restartNeeded = false)
        assertEquals("The agent now starts with the car.", failed.status)
        assertEquals("The head unit couldn't remove everything. The head unit's page still has it.", failed.note)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.AutostartStage
import com.nbtapps.store.data.jobs.AutostartStage.CHECKING
import com.nbtapps.store.data.jobs.AutostartStage.GETTING_PLUGIN
import com.nbtapps.store.data.jobs.AutostartStage.INSTALLING_PLUGIN
import com.nbtapps.store.data.jobs.AutostartStage.SWITCHING_ON
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.model.CheckProblem
import com.nbtapps.store.model.CheckProblemFix
import com.nbtapps.store.ui.headunits.ChecklistMark.DONE
import com.nbtapps.store.ui.headunits.ChecklistMark.FAILED
import com.nbtapps.store.ui.headunits.ChecklistMark.TO_COME
import com.nbtapps.store.ui.headunits.ChecklistMark.WORKING
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The set-up that has the agent start with the car, as the checklist shows it. */
class AutostartChecklistTest {

    private fun checklist(step: AutostartStep, vararg stages: AutostartStage, restartNeeded: Boolean = false) =
        autostartChecklist(AutostartSetupState("car", step, stages.toList()), restartNeeded)

    private fun marks(text: AutostartChecklistText) = text.rows.map { it.stage to it.mark }

    @Test
    fun `while the head unit is looked at, nothing is said yet of what comes after`() {
        val text = checklist(AutostartStep.Checking, CHECKING)
        assertEquals(listOf(ChecklistRow(CHECKING, "Checking the head unit…", WORKING)), text.rows)
        assertNull(text.end)
    }

    @Test
    fun `a plugin that is wanting has its rows, done, working and to come, in the card's words while they run`() {
        // The plugin is chosen while the card still says it is checking.
        assertEquals(
            listOf(
                ChecklistRow(CHECKING, "Checked the head unit", DONE),
                ChecklistRow(GETTING_PLUGIN, "Getting the iDrive plugin…", WORKING),
                ChecklistRow(INSTALLING_PLUGIN, "Installing the iDrive plugin", TO_COME),
                ChecklistRow(SWITCHING_ON, "Switching on", TO_COME),
            ),
            checklist(AutostartStep.Checking, CHECKING, GETTING_PLUGIN).rows,
        )
        assertEquals(
            listOf(CHECKING to DONE, GETTING_PLUGIN to WORKING, INSTALLING_PLUGIN to TO_COME, SWITCHING_ON to TO_COME),
            marks(checklist(AutostartStep.Downloading, CHECKING, GETTING_PLUGIN)),
        )
        val installing = checklist(AutostartStep.Installing("2021222324252627"), CHECKING, GETTING_PLUGIN, INSTALLING_PLUGIN)
        assertEquals(listOf(CHECKING to DONE, GETTING_PLUGIN to DONE, INSTALLING_PLUGIN to WORKING, SWITCHING_ON to TO_COME), marks(installing))
        assertEquals(autostartSetupText(AutostartStep.Installing(), restartNeeded = false).status, installing.rows[2].text)

        val done = checklist(AutostartStep.Done(changed = true), *AutostartStage.entries.toTypedArray(), restartNeeded = true)
        assertEquals(listOf("Checked the head unit", "Got the iDrive plugin", "Installed the iDrive plugin", "Switched on"), done.rows.map { it.text })
        assertTrue(done.rows.all { it.mark == DONE })
        assertEquals(autostartSetupText(AutostartStep.Done(changed = true), restartNeeded = true), done.end)
        assertEquals(AUTOSTART_RESTART_NEEDED, done.end?.restart)
    }

    @Test
    fun `a set-up that only switches on, or finds it on already, never shows the plugin as got or installed`() {
        assertEquals(listOf(CHECKING to DONE, SWITCHING_ON to WORKING), marks(checklist(AutostartStep.SwitchingOn, CHECKING, SWITCHING_ON)))

        val switched = checklist(AutostartStep.Done(changed = true), CHECKING, SWITCHING_ON)
        assertEquals(listOf(CHECKING to DONE, SWITCHING_ON to DONE), marks(switched))
        assertEquals("The agent now starts with the car.", switched.end?.status)

        val already = checklist(AutostartStep.Done(changed = false), CHECKING)
        assertEquals(listOf(CHECKING to DONE), marks(already))
        assertEquals("The agent already starts with the car.", already.end?.status)
    }

    @Test
    fun `a set-up that stopped marks the stage it stopped in, and gives the reason with Retry`() {
        val failed = checklist(AutostartStep.Failed("The iDrive plugin couldn't be downloaded: no network"), CHECKING, GETTING_PLUGIN)
        assertEquals(
            listOf(ChecklistRow(CHECKING, "Checked the head unit", DONE), ChecklistRow(GETTING_PLUGIN, "Getting the iDrive plugin", FAILED)),
            failed.rows,
        )
        assertEquals("The iDrive plugin couldn't be downloaded: no network", failed.end?.status)
        assertTrue(failed.end!!.failed)
        assertNull("nothing to come once it stopped", failed.rows.firstOrNull { it.mark == TO_COME })

        assertEquals(listOf(CHECKING to FAILED), marks(checklist(AutostartStep.Failed("The app has no link to the head unit."), CHECKING)))
    }

    private val demo = CheckProblem(
        id = "aa-fix",
        title = "An app that was installed by hand",
        detail = "It stands in the way.",
        paths = listOf("/opt/demo/bin/demod"),
        fix = CheckProblemFix("Remove that app", "Removes it.", "hmi-restart"),
    )

    @Test
    fun `the question about what was installed by hand, and its fix, come under the rows as the card has them`() {
        val asked = AutostartStep.AskRemoval(demo, switchedOn = true)
        val asking = checklist(asked, CHECKING, SWITCHING_ON, restartNeeded = true)
        assertEquals(listOf(CHECKING to DONE, SWITCHING_ON to DONE), marks(asking))
        assertEquals(autostartSetupText(asked, restartNeeded = true), asking.end)
        assertEquals("Remove that app", asking.end?.fixLabel)
        assertNull(asking.end?.restart)

        val removing = AutostartStep.Removing(demo, switchedOn = false)
        val fixing = checklist(removing, CHECKING)
        assertEquals(listOf(CHECKING to DONE), marks(fixing))
        assertEquals(autostartSetupText(removing, restartNeeded = false), fixing.end)
        assertTrue(fixing.end!!.working)

        val restarting = checklist(AutostartStep.Restarting, CHECKING, SWITCHING_ON, restartNeeded = true)
        assertEquals(AUTOSTART_RESTARTING, restarting.end?.status)
    }

    @Test
    fun `a state without stages, as one made by hand, still has its first row`() {
        assertEquals(listOf(CHECKING to WORKING), marks(autostartChecklist(AutostartSetupState("car", AutostartStep.Checking), restartNeeded = false)))
    }
}

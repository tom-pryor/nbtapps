// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.AutostartStage
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.ui.headunits.AUTOSTART_RESTARTING
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** What the start-with-the-car and Done pages say and offer, by how the set-up to start with the car stands. */
class FinishTextTest {

    private val name = "BMW 12345"

    private fun setup(step: AutostartStep, vararg stages: AutostartStage) = AutostartSetupState("car", step, stages.toList())

    private val installed = setup(
        AutostartStep.Done(changed = true),
        AutostartStage.CHECKING,
        AutostartStage.GETTING_PLUGIN,
        AutostartStage.INSTALLING_PLUGIN,
        AutostartStage.SWITCHING_ON,
    )
    private val switched = setup(AutostartStep.Done(changed = true), AutostartStage.CHECKING, AutostartStage.SWITCHING_ON)

    @Test
    fun `Done says what is left to do, case by case`() {
        assertEquals(AUTOSTART_RESTARTING, doneText(name, setup(AutostartStep.Restarting), restartNeeded = false, startsWithCar = true))
        val restart = "iDrive has to restart before the agent starts with the car. Restart it from BMW 12345's page when it suits."
        assertEquals(restart, doneText(name, switched, restartNeeded = true, startsWithCar = true))
        // The plugin went in on this run: iDrive has to restart, even once the link is gone and cannot say so.
        assertEquals(restart, doneText(name, installed, restartNeeded = false, startsWithCar = false))
        assertEquals("The agent now starts with the car.", doneText(name, switched, restartNeeded = false, startsWithCar = true))
        assertEquals(
            "The agent already starts with the car.",
            doneText(name, setup(AutostartStep.Done(changed = false), AutostartStage.CHECKING), restartNeeded = false, startsWithCar = true),
        )
        assertTrue(doneText(name, setup(AutostartStep.Failed("No link"), AutostartStage.CHECKING), false, false).endsWith("BMW 12345's page shows where it stands."))
        assertTrue(doneText(name, setup(AutostartStep.SwitchingOn, AutostartStage.SWITCHING_ON), false, false).endsWith("BMW 12345's page shows where it stands."))
        assertEquals("Autostart is off: start the agent on the car each time before you connect.", doneText(name, null, false, false))
        // A phone that was paired already, with an agent that starts with the car already.
        assertEquals("The agent already starts with the car.", doneText(name, null, restartNeeded = false, startsWithCar = true))
    }

    @Test
    fun `the agent starts with the car where the set-up got there, or where the link said so with no set-up`() {
        assertTrue(startsWithCarNow(switched, startsWithCar = false))
        assertTrue(startsWithCarNow(setup(AutostartStep.Restarting), startsWithCar = false))
        assertFalse(startsWithCarNow(setup(AutostartStep.Failed("No link")), startsWithCar = true))
        assertFalse(startsWithCarNow(setup(AutostartStep.Checking), startsWithCar = true))
        assertTrue(startsWithCarNow(null, startsWithCar = true))
        assertFalse(startsWithCarNow(null, startsWithCar = false))
    }

    @Test
    fun `moving on is Not now while the restart is offered, Continue once ended otherwise, and nothing while it runs`() {
        assertEquals("Not now", moveOnLabel(installed, restartNeeded = true))
        assertEquals("Continue", moveOnLabel(installed, restartNeeded = false))
        assertEquals("Continue", moveOnLabel(setup(AutostartStep.Failed("No link")), restartNeeded = false))
        assertNull(moveOnLabel(setup(AutostartStep.Installing()), restartNeeded = true))
        assertNull(moveOnLabel(setup(AutostartStep.Checking), restartNeeded = false))
    }
}

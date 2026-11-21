// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.SerialEndpoint
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Which page the set-up shows for what the user answered, the app's link and the set-up to start with the car. */
class SetupStepTest {

    private val serial = SerialEndpoint("AA:BB:CC:DD:EE:FF", "BMW 12345")
    private val picked = SetupAnswers(agentInstalled = true, headUnitId = "car")

    private fun connected(id: String) =
        ConnectionState.Connected(id, serial, AgentInfo("1.4.0", "000102030405060708090a0b0c0d0e0f", 1), emptyList())

    private fun waiting(id: String, stale: Boolean = false, autostart: Boolean = true) = ConnectionState.NeedsPairing(
        id,
        serial,
        stale,
        if (autostart) listOf(ProtocolConstants.CAPABILITY_AUTOSTART) else emptyList(),
    )

    private fun setup(id: String, step: AutostartStep = AutostartStep.Checking) = AutostartSetupState(id, step)

    @Test
    fun `before a car is picked the answers alone decide`() {
        assertEquals(SetupStep.Welcome, setupStep(SetupAnswers(), ConnectionState.Disconnected, null))
        assertEquals(SetupStep.NotYet, setupStep(SetupAnswers(agentInstalled = false), ConnectionState.Disconnected, null))
        assertEquals(SetupStep.PickCar, setupStep(SetupAnswers(agentInstalled = true), ConnectionState.Disconnected, null))
        // A link the app has to some car, and a set-up, change none of it.
        assertEquals(SetupStep.PickCar, setupStep(SetupAnswers(agentInstalled = true), connected("car"), setup("car")))
        assertEquals(SetupStep.Welcome, setupStep(SetupAnswers(), connected("car"), null))
    }

    @Test
    fun `with a car picked the link to it decides until the phone is paired`() {
        assertEquals(SetupStep.Connect(), setupStep(picked, ConnectionState.Connecting("car"), null))
        assertEquals(SetupStep.Connect("No agent answered"), setupStep(picked, ConnectionState.Failed("car", "No agent answered"), null))
        // A connect that only waited for the car's link offers to connect from the phone; while the next is under way, not.
        assertEquals(
            SetupStep.Connect("No link came", fromPhone = true),
            setupStep(picked, ConnectionState.Failed("car", "No link came", listenedOnly = true), null),
        )
        assertEquals(SetupStep.Connect(), setupStep(picked, ConnectionState.Failed("car", "No link came", listenedOnly = true), null, connecting = true))
        assertEquals(SetupStep.Connect(NO_LINK), setupStep(picked, ConnectionState.Failed("other", "No link came", listenedOnly = true), null))
        assertEquals(SetupStep.Pair(stale = false, offersAutostart = true), setupStep(picked, waiting("car"), null))
        assertEquals(SetupStep.Pair(stale = true, offersAutostart = false), setupStep(picked, waiting("car", stale = true, autostart = false), null))
    }

    @Test
    fun `the set-up's own connect shows as connecting, and with none under way no link is said as such`() {
        assertEquals(SetupStep.Connect(), setupStep(picked, ConnectionState.Disconnected, null, connecting = true))
        // The last try's failure stands until the next one has begun.
        assertEquals(SetupStep.Connect(), setupStep(picked, ConnectionState.Failed("car", "No agent answered"), null, connecting = true))
        assertEquals(SetupStep.Connect(NO_LINK), setupStep(picked, ConnectionState.Disconnected, null))
    }

    @Test
    fun `a link that is up is a paired phone, and Pair is not shown`() {
        assertEquals(SetupStep.Done, setupStep(picked, connected("car"), null))
        assertEquals(SetupStep.StartWithCar, setupStep(picked, connected("car"), setup("car")))
        // Straight after the connect, or paired meanwhile by another app's code: the same.
        assertEquals(SetupStep.StartWithCar, setupStep(picked, connected("car"), setup("car"), connecting = true))
    }

    @Test
    fun `once paired the page no longer depends on the link`() {
        val paired = picked.copy(paired = true)
        for (link in listOf(ConnectionState.Disconnected, ConnectionState.Failed("car", "iDrive restarted"), ConnectionState.Connecting("car"), waiting("car"))) {
            assertEquals(SetupStep.StartWithCar, setupStep(paired, link, setup("car", AutostartStep.Done(changed = true))))
            assertEquals(SetupStep.Done, setupStep(paired, link, null))
        }
    }

    @Test
    fun `start with the car shows while there is a set-up and the user has not moved on`() {
        val paired = picked.copy(paired = true)
        assertEquals(SetupStep.StartWithCar, setupStep(paired, connected("car"), setup("car", AutostartStep.Failed("No link"))))
        assertEquals(SetupStep.Done, setupStep(paired.copy(movedOn = true), connected("car"), setup("car")))
        // The restart was taken: on to Done.
        assertEquals(SetupStep.Done, setupStep(paired, ConnectionState.Disconnected, setup("car", AutostartStep.Restarting)))
    }

    @Test
    fun `a link or a set-up that is another head unit's counts as none`() {
        assertEquals(SetupStep.Connect(NO_LINK), setupStep(picked, connected("other"), null))
        assertEquals(SetupStep.Connect(NO_LINK), setupStep(picked, waiting("other"), null))
        assertEquals(SetupStep.Connect(NO_LINK), setupStep(picked, ConnectionState.Failed("other", "No agent answered"), null))
        assertEquals(SetupStep.Connect(), setupStep(picked, ConnectionState.Connecting("other"), null, connecting = true))
        assertEquals(SetupStep.Done, setupStep(picked, connected("car"), setup("other")))
    }

    @Test
    fun `while a code is with the agent the Pair page stays, so the set-up it begins is there first`() {
        assertEquals(SetupStep.Pair(offersAutostart = true), setupStep(picked, waiting("car"), null, pairing = true))
        // The agent took the code: the link is up, and the set-up is still to be begun.
        val reached = picked.copy(offersAutostart = true)
        assertEquals(SetupStep.Pair(offersAutostart = true), setupStep(reached, connected("car"), null, pairing = true))
        assertEquals(SetupStep.StartWithCar, setupStep(reached.copy(paired = true), connected("car"), setup("car")))
    }

    @Test
    fun `four steps, or three once the agent turns out to have no autostart or the box is cleared`() {
        assertEquals(4, setupStepCount(SetupAnswers()))
        // Before the Pair page has been reached the box says nothing yet.
        assertEquals(4, setupStepCount(picked.copy(enableAutostart = false)))
        assertEquals(4, setupStepCount(picked.copy(offersAutostart = true)))
        assertEquals(3, setupStepCount(picked.copy(offersAutostart = true, enableAutostart = false)))
        assertEquals(3, setupStepCount(picked.copy(offersAutostart = false)))
    }

    @Test
    fun `the numbered pages are the four between Welcome and Done`() {
        assertEquals(1, setupStepNumber(SetupStep.PickCar))
        assertEquals(2, setupStepNumber(SetupStep.Connect("No agent answered")))
        assertEquals(3, setupStepNumber(SetupStep.Pair()))
        assertEquals(4, setupStepNumber(SetupStep.StartWithCar))
        assertNull(setupStepNumber(SetupStep.Welcome))
        assertNull(setupStepNumber(SetupStep.NotYet))
        assertNull(setupStepNumber(SetupStep.Done))
    }
}

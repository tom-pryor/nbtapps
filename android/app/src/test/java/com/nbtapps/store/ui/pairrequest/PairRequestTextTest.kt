// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.ui.headunits.ChecklistMark
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** What the screen for a handed-over code says, page by page. */
class PairRequestTextTest {

    private val car = HeadUnit("car", "BMW 12345", listOf(SerialEndpoint("AA:BB:CC:DD:EE:FF", "BMW 12345")))
    private val other = HeadUnit("other", "Other car", listOf(SerialEndpoint("11:22:33:44:55:66", "Other car")))

    private fun state(step: PairRequestStep, remainingMs: Long? = 60_000, units: List<HeadUnit> = listOf(car), chosen: HeadUnit? = car) =
        PairRequestUiState(loaded = true, code = "042517", headUnits = units, chosen = chosen, remainingMs = remainingMs, step = step)

    @Test
    fun `the prompt's first line explains the app only where nothing is saved`() {
        assertEquals("Another app sent a pairing code.", promptLead(anySaved = true))
        assertTrue(promptLead(anySaved = false).startsWith("Another app sent a pairing code. NBTApps installs apps"))
    }

    @Test
    fun `Pair names the car, and is plain with none chosen`() {
        assertEquals("Pair with BMW 12345", pairLabel("BMW 12345"))
        assertEquals("Pair", pairLabel(null))
    }

    @Test
    fun `the code's card counts down while the code is good, and is gone once the phone is paired`() {
        assertEquals(CodeCard("042 517", "1:00", ranOut = false), codeCard(state(PairRequestStep.Prompt)))
        assertEquals(CodeCard("042 517", null, ranOut = false), codeCard(state(PairRequestStep.Prompt, remainingMs = null)))
        assertEquals(CodeCard("042 517", "1:00", ranOut = false), codeCard(state(PairRequestStep.Pairing)))
        assertEquals(CodeCard("042 517", null, ranOut = true), codeCard(state(PairRequestStep.TimeUp, remainingMs = 0)))
        // Run out by a code with no time given: the agent said so.
        assertEquals(CodeCard("042 517", null, ranOut = true), codeCard(state(PairRequestStep.TimeUp, remainingMs = null)))
        assertEquals(CodeCard("042 517", null, ranOut = false), codeCard(state(PairRequestStep.Refused("No."))))
        assertEquals(CodeCard("042 517", null, ranOut = true), codeCard(state(PairRequestStep.NoLink("No route"), remainingMs = 0)))
        assertNull(codeCard(state(PairRequestStep.Paired("BMW 12345"))))
        assertNull(codeCard(state(PairRequestStep.AlreadyPaired("BMW 12345"))))
    }

    @Test
    fun `the rows say the car was added only where this request added it, and follow the link then the code`() {
        val connecting = pairingRows(PairRequestStep.Connecting, newCar = true, name = "BMW 12345")
        assertEquals(
            listOf(
                PairingRow(ChecklistMark.DONE, "BMW 12345 added to the app"),
                PairingRow(ChecklistMark.WORKING, "Opening a link to the agent…", LINK_NOTE),
                PairingRow(ChecklistMark.TO_COME, "Sending the code"),
            ),
            connecting,
        )
        val sending = pairingRows(PairRequestStep.Pairing, newCar = false, name = "BMW 12345")
        assertEquals(
            listOf(
                PairingRow(ChecklistMark.DONE, "Opening a link to the agent…"),
                PairingRow(ChecklistMark.WORKING, "Sending the code"),
            ),
            sending,
        )
    }

    @Test
    fun `a set-up offers the line about iDrive's restart only where it offers the restart`() {
        val done = AutostartSetupState("car", AutostartStep.Done(changed = true))
        assertTrue(offersRestart(done, restartNeeded = true))
        assertFalse(offersRestart(done, restartNeeded = false))
        assertFalse(offersRestart(AutostartSetupState("car", AutostartStep.Checking), restartNeeded = true))
        assertFalse(offersRestart(null, restartNeeded = true))
    }

    @Test
    fun `a code that ran out names the car where one was chosen`() {
        val page = failedPage(state(PairRequestStep.TimeUp, remainingMs = 0), "BMW 12345")
        assertEquals(
            FailedPage(
                "This code ran out",
                "Nothing was sent to BMW 12345. Get a new code from the app that sent this one. A code can't be typed or changed here.",
                error = false,
                retry = false,
            ),
            page,
        )
        assertEquals(
            "Get a new code from the app that sent this one. A code can't be typed or changed here.",
            failedPage(state(PairRequestStep.TimeUp, remainingMs = 0, chosen = null), null)?.message,
        )
    }

    @Test
    fun `a refusal and a missing link keep their words, and Retry only while the code is good`() {
        val refused = failedPage(state(PairRequestStep.Refused("BMW 12345 didn't take the code.")), "BMW 12345")
        assertEquals(FailedPage("The code didn't pair this phone.", "BMW 12345 didn't take the code.", error = true, retry = false), refused)

        val noLink = failedPage(state(PairRequestStep.NoLink("No route to the car")), "BMW 12345")
        assertEquals(
            FailedPage("The code couldn't go to BMW 12345: the app has no link to it.", "No route to the car", error = true, retry = true),
            noLink,
        )
        assertFalse(failedPage(state(PairRequestStep.NoLink("No route to the car"), remainingMs = 0), "BMW 12345")!!.retry)
        assertNull(failedPage(state(PairRequestStep.Prompt), "BMW 12345"))
    }

    @Test
    fun `the car is said to stay only where it is saved and the only one`() {
        assertTrue(carStays(state(PairRequestStep.TimeUp)))
        assertFalse(carStays(state(PairRequestStep.TimeUp, units = listOf(car, other))))
        assertFalse(carStays(state(PairRequestStep.TimeUp, units = emptyList(), chosen = null)))
        val picked = state(PairRequestStep.TimeUp, units = emptyList(), chosen = null)
            .copy(picking = true, device = PairedDevice("BMW 12345", "AA:BB:CC:DD:EE:FF"))
        assertFalse(carStays(picked))
    }
}

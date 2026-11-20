// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairing

import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.model.AgentInfo
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.TcpEndpoint
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** What the pairing screen shows for each state of the app's link, and what it takes for a code. */
class PairingStepTest {

    private val tcp = TcpEndpoint("192.168.0.1", 7144)

    private fun connected(id: String) =
        ConnectionState.Connected(id, tcp, AgentInfo("1.0.0", "000102030405060708090a0b0c0d0e0f", 1), emptyList())

    @Test
    fun `the code is asked for only while the link to this head unit waits for it`() {
        assertEquals(PairingStep.EnterCode(stale = false), pairingStep("car", ConnectionState.NeedsPairing("car", tcp, stale = false)))
        assertEquals(PairingStep.EnterCode(stale = true), pairingStep("car", ConnectionState.NeedsPairing("car", tcp, stale = true)))

        assertEquals(PairingStep.Connecting, pairingStep("car", ConnectionState.Connecting("car")))
        // A link the agent answers on is one of a paired phone.
        assertEquals(PairingStep.Paired, pairingStep("car", connected("car")))
        assertEquals(PairingStep.NoLink(null), pairingStep("car", ConnectionState.Disconnected))
        assertEquals(PairingStep.NoLink("No route to the car"), pairingStep("car", ConnectionState.Failed("car", "No route to the car")))
        // A connect that only waited for the car's link offers the phone's own.
        assertEquals(
            PairingStep.NoLink("No link came", fromPhone = true),
            pairingStep("car", ConnectionState.Failed("car", "No link came", listenedOnly = true)),
        )
        assertEquals(PairingStep.NoLink(null), pairingStep("car", ConnectionState.Failed("van", "No link came", listenedOnly = true)))
    }

    @Test
    fun `the agent's capability decides whether starting with the car is offered`() {
        val with = listOf("files", ProtocolConstants.CAPABILITY_AUTOSTART)
        assertEquals(
            PairingStep.EnterCode(stale = false, autostart = true),
            pairingStep("car", ConnectionState.NeedsPairing("car", tcp, stale = false, capabilities = with)),
        )
        assertEquals(
            PairingStep.EnterCode(stale = true, autostart = true),
            pairingStep("car", ConnectionState.NeedsPairing("car", tcp, stale = true, capabilities = with)),
        )
        // An agent from before the switch has nothing to set up.
        val without = pairingStep("car", ConnectionState.NeedsPairing("car", tcp, stale = false, capabilities = listOf("files")))
        assertEquals(PairingStep.EnterCode(stale = false, autostart = false), without)

        // Pairing goes on to the set-up only with the box shown and ticked.
        assertTrue(setsUpAutostart(ticked = true, PairingStep.EnterCode(stale = false, autostart = true)))
        assertFalse(setsUpAutostart(ticked = false, PairingStep.EnterCode(stale = false, autostart = true)))
        assertFalse(setsUpAutostart(ticked = true, without))
        assertFalse(setsUpAutostart(ticked = true, PairingStep.Paired))
    }

    @Test
    fun `a link to another head unit is none to this one`() {
        assertEquals(PairingStep.NoLink(null), pairingStep("car", ConnectionState.NeedsPairing("van", tcp, stale = false)))
        assertEquals(PairingStep.NoLink(null), pairingStep("car", connected("van")))
        assertEquals(PairingStep.NoLink(null), pairingStep("car", ConnectionState.Connecting("van")))
        assertEquals(PairingStep.NoLink(null), pairingStep("car", ConnectionState.Failed("van", "No route")))
    }

    @Test
    fun `a code is six digits as the agent reads them, however they are typed`() {
        assertEquals("951120", codeDigits("951120"))
        assertEquals("951120", codeDigits("951 120"))
        assertEquals("951120", codeDigits("951-120 7"))
        assertEquals("95", codeDigits("9a5"))
        // Digits of another script are not the agent's.
        assertEquals("", codeDigits("٩٥١١٢٠"))
    }

    @Test
    fun `the code can be sent once it is whole, the link waits for it and none is on its way`() {
        val waiting = PairingUiState(loaded = true, headUnitName = "Car", step = PairingStep.EnterCode(stale = false))
        assertFalse(waiting.canPair)
        assertFalse(waiting.copy(code = "95112").canPair)
        assertTrue(waiting.copy(code = "951120").canPair)
        assertFalse(waiting.copy(code = "951120", working = true).canPair)
        assertFalse(waiting.copy(code = "951120", step = PairingStep.NoLink(null)).canPair)
    }

    @Test
    fun `what the checks found is pointed to once paired, where no set-up asks about it, and Done is not held`() {
        val paired = PairingUiState(loaded = true, headUnitName = "Car", step = PairingStep.Paired, openChecks = 2)
        assertTrue(paired.showsChecks)
        assertFalse(paired.copy(openChecks = 0).showsChecks)
        // A set-up that runs or is shown asks about them itself.
        assertFalse(paired.copy(setup = AutostartStep.Checking).showsChecks)
        assertFalse(paired.copy(setup = AutostartStep.Done(changed = true)).showsChecks)
        assertFalse(paired.copy(step = PairingStep.EnterCode(stale = false)).showsChecks)
    }

    @Test
    fun `a refused code is told apart from a refused request`() {
        for (code in listOf("forbidden", "rejected", "bad-request", "unsupported")) {
            assertNotEquals(code, AgentText.request(code), AgentText.pairing(code))
        }
        assertTrue(AgentText.pairing("forbidden").contains("Pair a phone"))
        // Anything else is said as for any request.
        assertEquals(AgentText.request("internal"), AgentText.pairing("internal"))
        assertEquals(AgentText.request("try-later"), AgentText.pairing("try-later"))
    }
}

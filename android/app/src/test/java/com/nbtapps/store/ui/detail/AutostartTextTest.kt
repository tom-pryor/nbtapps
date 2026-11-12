// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.AutostartReason
import com.nbtapps.store.model.PackageNeed
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** What the head unit's screen says under the switch that has the agent start with the car. */
class AutostartTextTest {

    private val needs = PackageNeed("org.nbtapp.plugin", ">=1.0.0")

    @Test
    fun `where it can be had the card says what on and off mean`() {
        assertEquals(
            "The iDrive plugin starts the agent each time the head unit starts.",
            autostartText(Autostart(on = true, available = true)),
        )
        assertTrue(autostartText(Autostart(on = false, available = true))!!.startsWith("Off: "))
        // One start on record is every start's for its first minutes, and is not spoken of.
        assertEquals(autostartText(Autostart(on = true, available = true)), autostartText(Autostart(on = true, available = true, starts = 1)))
    }

    @Test
    fun `starts that did not last are spoken of from the second, and only while it is on`() {
        val text = autostartText(Autostart(on = true, available = true, starts = 3))!!

        assertTrue(text, text.startsWith("3 starts with the car are on record"))
        assertEquals(autostartText(Autostart(on = false, available = true)), autostartText(Autostart(on = false, available = true, starts = 3)))
        assertEquals(2, AUTOSTART_STARTS_TO_SHOW)
    }

    @Test
    fun `where nothing does the starting the card says why, and that a switch left on does nothing`() {
        val missing = Autostart(on = false, available = false, reason = AutostartReason.PLUGIN_MISSING, needs = needs)
        assertEquals(AgentText.autostartReason(AutostartReason.PLUGIN_MISSING), autostartText(missing))
        assertEquals(
            "It is switched on, but nothing starts the agent. " + AgentText.autostartReason(AutostartReason.PLUGIN_MISSING),
            autostartText(missing.copy(on = true)),
        )

        // Each reason has words of its own, and one the app does not know has some all the same.
        val reasons = listOf(AutostartReason.PLUGIN_MISSING, AutostartReason.PLUGIN_TOO_OLD, AutostartReason.NO_LAUNCHER, "one from later", null)
        assertEquals(4, reasons.map(AgentText::autostartReason).distinct().size)
        assertEquals(AgentText.autostartReason("one from later"), AgentText.autostartReason(null))
    }

    @Test
    fun `a refused switch is put in the app's words`() {
        for (reason in listOf(AutostartReason.PLUGIN_MISSING, AutostartReason.PLUGIN_TOO_OLD, AutostartReason.NO_LAUNCHER)) {
            assertEquals(AgentText.autostartReason(reason), AgentText.autostart(reason))
        }
        // An agent on trial after its own update writes nothing until it is confirmed.
        assertNotEquals(AgentText.request("busy"), AgentText.autostart("busy"))
        assertEquals(AgentText.request("forbidden"), AgentText.autostart("forbidden"))
    }
}

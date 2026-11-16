// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairing

import org.junit.Assert.assertEquals
import org.junit.Test

/** What the code's form says for a car whose screen has no entry to pair a phone from. */
class PairingCodeFormTest {

    @Test
    fun `the help gives the two commands that get a code over SSH, in order`() {
        assertEquals("No NBTApp entry on the car's screen?", NO_ENTRY_HELP)
        assertEquals(
            listOf(
                HelpLine(
                    "The iDrive plugin isn't on this car yet. Get a code over SSH instead, in a shell on the head unit. " +
                        "First see which version of the agent is installed:",
                ),
                HelpLine("cat /var/opt/nbtapp/agent/current", command = true),
                HelpLine("Then ask it for a code, with that version in place of 1.4.0:"),
                HelpLine("/var/opt/nbtapp/agent/1.4.0/nbtctl pair", command = true),
                HelpLine("With autostart on, the app installs the plugin once the phone is paired, so this is needed only once."),
            ),
            NO_ENTRY_HELP_TEXT,
        )
    }
}

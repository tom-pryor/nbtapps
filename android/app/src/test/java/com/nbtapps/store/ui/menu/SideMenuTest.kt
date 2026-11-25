// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.menu

import androidx.compose.material3.DrawerValue.Closed
import androidx.compose.material3.DrawerValue.Open
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** When the side menu counts as in sight, and when it has to close for the screen on top. */
class SideMenuTest {

    @Test
    fun `the menu shows from the tap that opens it until it has gone`() {
        assertFalse(menuShows(current = Closed, target = Closed))
        assertTrue("on its way in", menuShows(current = Closed, target = Open))
        assertTrue(menuShows(current = Open, target = Open))
        assertTrue("on its way out", menuShows(current = Open, target = Closed))
    }

    @Test
    fun `the menu gives way to a screen that is not a tab, and stays over a tab`() {
        assertTrue(menuGivesWay(screenKnown = true, tabOnTop = false))
        assertFalse(menuGivesWay(screenKnown = true, tabOnTop = true))
    }

    @Test
    fun `the menu stays as it is while the screen on top is not known, as just after a rotation`() {
        assertFalse(menuGivesWay(screenKnown = false, tabOnTop = false))
    }
}

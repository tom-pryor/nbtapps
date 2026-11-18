// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.InstalledPackage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * An installed package's row says that it uses features this version of the app does not know: its installed
 * version is on the phone with a manifest of a later format, or the agent gives it a flag the app cannot explain.
 */
class LaterFeaturesRowTest {

    private fun stored(id: String, version: String, laterFormat: Boolean) = StoredPackage(
        id = id, name = id, version = version, sha256 = "$id-$version", size = 1, manifestSha256 = "11", signature = "22",
        key = "33", laterFormat = laterFormat,
    )

    private val dial = InstalledPackage("org.nbtapp.dial", "Dial", "1.1.0")
    private val radio = InstalledPackage("org.nbtapp.radio", "Radio", "1.0.0", flags = listOf("runs-scripts"))

    @Test
    fun `the installed version's archive on the phone says whether its manifest is of a later format`() {
        val stored = listOf(
            stored("org.nbtapp.dial", "1.0.0", laterFormat = true),
            stored("org.nbtapp.dial", "1.1.0", laterFormat = true),
            stored("org.nbtapp.radio", "1.0.0", laterFormat = false),
        )
        assertEquals(setOf("org.nbtapp.dial"), laterFormat(listOf(dial, radio), stored))
        // Only the version that is installed counts.
        assertEquals(emptySet<String>(), laterFormat(listOf(dial.copy(version = "1.2.0")), stored))
        assertEquals(emptySet<String>(), laterFormat(listOf(dial), emptyList()))
    }

    @Test
    fun `the row says so for such a package, and for a flag of the agent's that this app cannot explain`() {
        val state = HeadUnitDetailUiState(installed = listOf(dial, radio), laterFormat = setOf("org.nbtapp.dial"))
        assertTrue(laterFeatures(state, dial))
        assertFalse(laterFeatures(state, radio))
        assertTrue(laterFeatures(state, radio.copy(flags = listOf("runs-scripts", "opens-ports"))))
    }
}

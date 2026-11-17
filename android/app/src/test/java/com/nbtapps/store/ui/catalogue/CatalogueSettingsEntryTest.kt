// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.RestartKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The Settings button at the right of a package page's "Installed" card: there where the installed package has
 * settings, with or without a link, and usable only with one.
 */
class CatalogueSettingsEntryTest {

    private fun version(version: String) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = version.padEnd(8, '0'),
        manifestSha256 = "11",
    )

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test source"),
        serial = 1,
        packages = listOf(
            CataloguePackage(id = "org.nbtapp.dial", name = "Dial", versions = listOf(version("1.0.0"), version("1.1.0"))),
            CataloguePackage(id = "org.nbtapp.plain", name = "Plain", versions = listOf(version("1.0.0"))),
        ),
    )

    private fun held(live: Boolean, vararg installed: InstalledPackage) =
        Holdings("car", "Car", live = live, agentVersion = "1.5.0", installed = installed.toList(), restartNeeded = RestartKind.NONE, seenAt = 1)

    private fun get(id: String, head: Holdings?): GetState =
        requireNotNull(cataloguePackageUiState(CatalogueState(index = index), id, DetailsState.Loading, emptyList(), emptyMap(), head, emptySet()).get)

    private val dial = InstalledPackage("org.nbtapp.dial", "Dial", "1.0.0", settings = true)

    @Test
    fun `an installed package with settings has the button, usable with a link to its head unit`() {
        val live = get("org.nbtapp.dial", held(live = true, dial))
        assertTrue(live.settings && live.connected)
        assertEquals("car", live.headUnitId)
        assertEquals("1.0.0", live.installedVersion)

        // With no link, as the head unit said the last time: there, and not usable.
        val offline = get("org.nbtapp.dial", held(live = false, dial))
        assertTrue(offline.settings)
        assertFalse(offline.connected)
    }

    @Test
    fun `a package not installed, or installed without settings, has none`() {
        assertFalse(get("org.nbtapp.dial", held(live = true)).settings)
        assertFalse(get("org.nbtapp.dial", null).settings)
        assertFalse(get("org.nbtapp.plain", held(live = true, InstalledPackage("org.nbtapp.plain", "Plain", "1.0.0"))).settings)
        // Another package's settings are not this one's.
        assertFalse(get("org.nbtapp.plain", held(live = true, dial, InstalledPackage("org.nbtapp.plain", "Plain", "1.0.0"))).settings)
    }
}

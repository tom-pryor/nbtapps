// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.RiskFlag
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Where the app says that a package uses features this version of it does not know: a package page whose offered
 * version is on the phone with a manifest of a later format, and a risk flag it cannot explain, from the catalogue
 * or from the agent's plan, which is never left out unsaid.
 */
class LaterFeaturesTest {

    private fun version(version: String, vararg flags: String) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = version.padEnd(8, '0'),
        manifestSha256 = "11",
        flags = flags.toList(),
    )

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test source"),
        serial = 1,
        packages = listOf(CataloguePackage(id = "org.nbtapp.dial", name = "Dial", versions = listOf(version("1.1.0"), version("1.0.0")))),
    )

    private val head = Holdings("car", "Car", live = true, agentVersion = "1.6.0", installed = emptyList(), restartNeeded = RestartKind.NONE, seenAt = 1)

    private fun stored(version: String, laterFormat: Boolean) = StoredPackage(
        id = "org.nbtapp.dial", name = "Dial", version = version, sha256 = version.padEnd(8, '0'), size = 1,
        manifestSha256 = "11", signature = "22", key = "33", laterFormat = laterFormat,
    )

    private fun get(vararg stored: StoredPackage): GetState = requireNotNull(
        cataloguePackageUiState(CatalogueState(index = index), "org.nbtapp.dial", DetailsState.Loading, stored.toList(), emptyMap(), head, emptySet()).get,
    )

    @Test
    fun `the page says so while the offered version is on the phone with a manifest of a later format`() {
        assertFalse(get().laterFormat)
        assertFalse(get(stored("1.1.0", laterFormat = false)).laterFormat)
        val later = get(stored("1.1.0", laterFormat = true))
        assertTrue(later.stored && later.laterFormat)
        // Another version's archive says nothing of the one on offer.
        assertFalse(get(stored("1.0.0", laterFormat = true)).laterFormat)
    }

    @Test
    fun `a risk flag this app does not know is noticed, from the catalogue and from the agent's plan`() {
        assertFalse(version("1.0.0").unknownFlags)
        assertFalse(version("1.0.0", "runs-scripts", "needs-reboot").unknownFlags)
        val later = version("1.0.0", "runs-scripts", "opens-ports")
        assertTrue(later.unknownFlags)
        // The flags this app knows are explained as before.
        assertEquals(listOf(RiskFlag.RUNS_SCRIPTS), later.riskFlags)

        assertFalse(Plan(flags = listOf("modifies-system")).unknownFlags)
        val plan = Plan(flags = listOf("modifies-system", "opens-ports"))
        assertTrue(plan.unknownFlags)
        assertEquals(listOf(RiskFlag.MODIFIES_SYSTEM), plan.riskFlags)
        assertTrue(RiskFlag.anyUnknown(listOf("opens-ports")) && !RiskFlag.anyUnknown(emptyList()))
    }
}

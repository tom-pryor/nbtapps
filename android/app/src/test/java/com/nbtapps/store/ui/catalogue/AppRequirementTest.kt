// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.jobs.Resolver
import com.nbtapps.store.data.jobs.packageStatus
import com.nbtapps.store.model.AppVersion
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageDependency
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.RestartKind
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * A version whose `requires.app` leaves this app out is not offered, by the resolver, the package's status, the
 * list's row or the package's page; where it holds back only a newer version, the row and the page say that it needs
 * a newer version of the app. As a version the head unit's agent is too old for is handled, as far as it goes.
 */
class AppRequirementTest {

    @Before
    fun thisApp() {
        AppVersion.current = "0.7.0"
    }

    @After
    fun forget() {
        AppVersion.current = null
    }

    private fun version(version: String, agent: String? = null, app: String? = null, vararg depends: PackageDependency) = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = version.padEnd(8, '0'),
        manifestSha256 = "11",
        requires = PackageRequirements(agent, app),
        depends = depends.toList(),
    )

    /** Versions newest first, as the catalogue parser leaves them. */
    private val dial = CataloguePackage(
        id = "org.nbtapp.dial",
        name = "Dial",
        versions = listOf(version("1.2.0", agent = ">=1.6.0", app = ">=0.8.0"), version("1.1.0", agent = ">=1.6.0"), version("1.0.0")),
    )
    private val newOnly = CataloguePackage(id = "org.nbtapp.new", name = "New", versions = listOf(version("1.0.0", app = ">=0.8.0")))
    private val user = CataloguePackage(
        id = "org.nbtapp.user",
        name = "User",
        versions = listOf(version("1.0.0", depends = arrayOf(PackageDependency("org.nbtapp.dial", ">=1.2.0")))),
    )
    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test source"),
        serial = 1,
        packages = listOf(dial, newOnly, user),
    )

    private fun held(agent: String) =
        Holdings("car", "Car", live = true, agentVersion = agent, installed = emptyList<InstalledPackage>(), restartNeeded = RestartKind.NONE, seenAt = 1)

    private fun get(id: String, head: Holdings?): GetState =
        requireNotNull(cataloguePackageUiState(CatalogueState(index = index), id, DetailsState.Loading, emptyList(), emptyMap(), head, emptySet()).get)

    @Test
    fun `a version this app is too old for suits no head unit, and none at all`() {
        assertFalse(Resolver.suits(dial.versions[0], "1.6.0"))
        assertFalse(Resolver.suits(dial.versions[0], null))
        assertTrue(Resolver.suitsAgent(dial.versions[0], "1.6.0"))
        assertTrue(Resolver.suits(dial.versions[1], "1.6.0"))
        // A newer app takes it.
        AppVersion.current = "0.8.0"
        assertTrue(Resolver.suits(dial.versions[0], "1.6.0"))
    }

    @Test
    fun `the newest version the app may install is offered, and the one above it is held back by the app alone`() {
        val status = requireNotNull(packageStatus(index, dial, held("1.6.0")))
        assertEquals("1.1.0", status.offered.version)
        assertTrue(status.suits && !status.needsApp)
        assertEquals("1.2.0", status.heldBack?.version)
        assertTrue(status.heldBackByApp)
        assertEquals("1.2.0 needs a newer version of the app", status.note())
        assertEquals("1.2.0", get("org.nbtapp.dial", held("1.6.0")).needsNewerApp)
        assertNull(get("org.nbtapp.dial", held("1.6.0")).incompatible)

        // Where the agent holds it back as well, the agent is named, as before.
        val older = requireNotNull(packageStatus(index, dial, held("1.5.1")))
        assertEquals("1.0.0", older.offered.version)
        assertFalse(older.heldBackByApp)
        assertEquals("1.2.0 needs agent 1.6.0 or later", older.note())
        assertNull(get("org.nbtapp.dial", held("1.5.1")).needsNewerApp)

        // A newer app is held back by nothing.
        AppVersion.current = "0.8.0"
        val newer = requireNotNull(packageStatus(index, dial, held("1.6.0")))
        assertEquals("1.2.0", newer.offered.version)
        assertNull(newer.heldBack)
        assertFalse(newer.heldBackByApp)
    }

    @Test
    fun `a package with no version this app may install says it needs a newer app, with or without a head unit`() {
        val status = requireNotNull(packageStatus(index, newOnly, held("1.6.0")))
        assertFalse(status.suits)
        assertTrue(status.needsApp)
        assertEquals(NEEDS_NEWER_APP, status.note())
        for (head in listOf(held("1.6.0"), null)) {
            val page = get("org.nbtapp.new", head)
            assertEquals("$NEEDS_NEWER_APP: 0.8.0 or later. This app is 0.7.0.", page.incompatible)
            assertFalse(page.canInstall)
        }
    }

    @Test
    fun `a dependency only a newer app may install is not taken`() {
        val resolution = Resolver.resolve(index, Resolver.Needed(user, user.versions[0]), emptyList(), "1.6.0")
        assertEquals(listOf("org.nbtapp.user"), resolution.needed.map { it.pkg.id })
        assertEquals(listOf("org.nbtapp.dial"), resolution.unavailable.map { it.id })
        AppVersion.current = "0.8.0"
        val newer = Resolver.resolve(index, Resolver.Needed(user, user.versions[0]), emptyList(), "1.6.0")
        assertEquals(listOf("org.nbtapp.dial", "org.nbtapp.user"), newer.needed.map { it.pkg.id })
    }
}

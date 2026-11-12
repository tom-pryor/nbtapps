// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.AgentUpdate
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.jobs.Unblocked
import com.nbtapps.store.data.jobs.packageStatus
import com.nbtapps.store.data.update.AppUpdateOffer
import com.nbtapps.store.data.update.UpdateState
import com.nbtapps.store.data.update.release
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.RestartKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The catalogue's list against a head unit ([catalogueUiState]), and the words the screen puts to it. */
class CatalogueListTest {

    private fun version(version: String, agent: String = ">=1.0.0") = PackageVersion(
        version = version,
        url = "https://example.invalid/$version",
        size = 1,
        sha256 = "00",
        manifestSha256 = "11",
        requires = PackageRequirements(agent),
    )

    private fun pkg(id: String, name: String, category: String, vararg versions: PackageVersion) =
        CataloguePackage(id = id, name = name, category = category, versions = versions.toList())

    private val index = CatalogueIndex(
        schema = 1,
        source = CatalogueSourceInfo("test", "Test source"),
        serial = 1,
        packages = listOf(
            pkg(AGENT_PACKAGE_ID, "NBTApp agent", "system", version("1.1.0")),
            pkg("runtime", "NBT Runtime", "libraries", version("2.1.0"), version("2.0.0")),
            pkg("hello", "Hello NBT", "utilities", version("1.2.0"), version("1.1.0")),
            pkg("launcher", "App Launcher", "hmi", version("0.3.0", ">=1.1.0"), version("0.2.1")),
            pkg("tzdata", "Time Zone Data", "data", version("2026.2.0"), version("2026.1.0")),
            pkg("btterm", "Bluetooth Terminal", "utilities", version("0.4.0")),
        ),
    )

    private fun has(id: String, version: String) = InstalledPackage(id, id, version)

    private val car = Holdings(
        headUnitId = "car",
        name = "Car",
        live = true,
        agentVersion = "1.0.2",
        installed = listOf(has("hello", "1.1.0"), has("tzdata", "2026.1.0"), has("launcher", "0.2.1"), has("runtime", "2.1.0")),
        restartNeeded = RestartKind.NONE,
        seenAt = 1,
    )

    private fun state(
        head: Holdings? = car,
        query: String = "",
        filter: CatalogueFilter = CatalogueFilter.All,
        refresh: HeadUnitRefresh = HeadUnitRefresh(),
        index: CatalogueIndex = this.index,
    ) = catalogueUiState(CatalogueState(index = index), head, query, filter, refresh)

    private fun CatalogueUiState.names(kind: CatalogueSection.Kind) = sections.filter { it.kind == kind }.flatMap { it.rows }.map { it.pkg.name }

    @Test
    fun `with no head unit known the list is the catalogue by name, and the head unit's filters fall back to everything`() {
        val alone = state(head = null)

        assertNull(alone.headUnit)
        assertEquals(listOf(CatalogueSection.Kind.PLAIN), alone.sections.map { it.kind })
        assertEquals(
            listOf("App Launcher", "Bluetooth Terminal", "Hello NBT", "NBT Runtime", "NBTApp agent", "Time Zone Data"),
            alone.packages.map { it.name },
        )
        assertEquals(0, alone.updateCount)
        assertNull(alone.agentUpdate)
        assertTrue(alone.sections.single().rows.all { it.status?.installed == null })

        assertEquals(CatalogueFilter.All, state(head = null, filter = CatalogueFilter.Installed).filter)
        assertEquals(CatalogueFilter.All, state(head = null, filter = CatalogueFilter.Updates).filter)
    }

    @Test
    fun `packages with an update come first, and the agent's has its card instead of a place among them`() {
        val all = state()

        assertEquals(listOf("Hello NBT", "Time Zone Data"), all.names(CatalogueSection.Kind.UPDATES))
        assertEquals(listOf("App Launcher", "Bluetooth Terminal", "NBT Runtime", "NBTApp agent"), all.names(CatalogueSection.Kind.REST))
        assertEquals(AgentUpdate("1.1.0", "1.0.2", listOf(Unblocked("launcher", "App Launcher", "0.3.0", installed = "0.2.1"))), all.agentUpdate)
        // The agent's update counts all the same, and its row still says so.
        assertEquals(3, all.updateCount)
        assertTrue(all.sections.last().rows.single { it.pkg.id == AGENT_PACKAGE_ID }.status!!.updateAvailable)
    }

    @Test
    fun `with nothing to update there is one list and no heading`() {
        val current = car.copy(
            agentVersion = "1.1.0",
            installed = listOf(has("hello", "1.2.0"), has("launcher", "0.3.0")),
        )
        val upToDate = state(head = current)

        assertEquals(listOf(CatalogueSection.Kind.PLAIN), upToDate.sections.map { it.kind })
        assertEquals(0, upToDate.updateCount)
        assertNull(upToDate.agentUpdate)
    }

    @Test
    fun `the updates filter shows every update, the agent's among them, and no card`() {
        val updates = state(filter = CatalogueFilter.Updates)

        assertEquals(listOf(CatalogueSection.Kind.UPDATES), updates.sections.map { it.kind })
        assertEquals(listOf("Hello NBT", "NBTApp agent", "Time Zone Data"), updates.packages.map { it.name })
        assertNull(updates.agentUpdate)
        assertEquals(3, updates.updateCount)
    }

    @Test
    fun `the installed filter shows what the head unit has, the agent that runs included, updates first`() {
        val installed = state(filter = CatalogueFilter.Installed)

        assertEquals(listOf("Hello NBT", "NBTApp agent", "Time Zone Data"), installed.names(CatalogueSection.Kind.UPDATES))
        assertEquals(listOf("App Launcher", "NBT Runtime"), installed.names(CatalogueSection.Kind.REST))
        assertNull(installed.agentUpdate)
    }

    @Test
    fun `a category narrows both sections, and one the catalogue has dropped stops filtering`() {
        val utilities = state(filter = CatalogueFilter.Category("utilities"))
        assertEquals(listOf("Hello NBT"), utilities.names(CatalogueSection.Kind.UPDATES))
        assertEquals(listOf("Bluetooth Terminal"), utilities.names(CatalogueSection.Kind.REST))
        assertEquals(listOf("data", "hmi", "libraries", "system", "utilities"), utilities.categories)
        // The count on the Updates chip is of the whole catalogue.
        assertEquals(3, utilities.updateCount)

        assertEquals(CatalogueFilter.All, state(filter = CatalogueFilter.Category("gone")).filter)
    }

    @Test
    fun `a search shows one list by name, with no heading and no card`() {
        val found = state(query = "nbt")

        assertEquals(listOf(CatalogueSection.Kind.PLAIN), found.sections.map { it.kind })
        assertEquals(listOf("Hello NBT", "NBT Runtime", "NBTApp agent"), found.packages.map { it.name })
        assertNull(found.agentUpdate)

        assertTrue(state(query = "nothing like this").sections.isEmpty())
    }

    @Test
    fun `a saved list marks the rows as a live one does`() {
        val saved = state(head = car.copy(live = false, seenAt = 99))

        assertEquals(listOf("Hello NBT", "Time Zone Data"), saved.names(CatalogueSection.Kind.UPDATES))
        assertFalse(saved.headUnit!!.live)
        assertEquals(99L, saved.headUnit!!.seenAt)
        assertEquals("1.0.2", saved.agentUpdate?.running)
    }

    @Test
    fun `reading the head unit again shows as refreshing, and its failure only while the link is there`() {
        val reading = state(refresh = HeadUnitRefresh(running = true))
        assertTrue(reading.refreshing)
        assertTrue(reading.refreshingHeadUnit)

        val failed = state(refresh = HeadUnitRefresh(error = "The head unit's agent is busy"))
        assertFalse(failed.refreshing)
        assertEquals("The head unit's agent is busy", failed.headUnitError)

        // The link went meanwhile: the top bar says so, and the list is the saved one.
        val gone = state(head = car.copy(live = false), refresh = HeadUnitRefresh(running = true, error = "The link was lost"))
        assertFalse(gone.refreshingHeadUnit)
        assertNull(gone.headUnitError)

        // The catalogue's own refresh shows the same way.
        assertTrue(catalogueUiState(CatalogueState(index = index, refreshing = true), car, "", CatalogueFilter.All, HeadUnitRefresh()).refreshing)
    }

    // ---- the screen's words ----

    @Test
    fun `an empty list says why it is empty`() {
        val noAgent = index.copy(packages = index.packages.drop(1))
        val bare = car.copy(installed = emptyList())

        assertNull(state().emptyMessage())
        assertEquals("No packages match.", state(query = "nothing like this").emptyMessage())
        assertEquals("No packages match.", state(query = "nothing like this", filter = CatalogueFilter.Installed).emptyMessage())
        assertEquals(
            "Nothing from this catalogue is installed on Car.",
            state(head = bare, filter = CatalogueFilter.Installed, index = noAgent).emptyMessage(),
        )
        assertEquals("No updates for Car.", state(head = bare, filter = CatalogueFilter.Updates, index = noAgent).emptyMessage())
        assertEquals("This catalogue has no packages.", state(index = index.copy(packages = emptyList())).emptyMessage())
        assertEquals("Couldn't load the catalogue: no network", catalogueUiState(CatalogueState(error = "no network"), car, "", CatalogueFilter.All, HeadUnitRefresh()).emptyMessage())
        assertNull(catalogueUiState(CatalogueState(), car, "", CatalogueFilter.All, HeadUnitRefresh()).emptyMessage())
    }

    @Test
    fun `the heading over the rest names what the list is narrowed to`() {
        assertNull(sectionTitle(CatalogueSection.Kind.PLAIN, CatalogueFilter.All))
        assertEquals("Updates available", sectionTitle(CatalogueSection.Kind.UPDATES, CatalogueFilter.Installed))
        assertEquals("All packages", sectionTitle(CatalogueSection.Kind.REST, CatalogueFilter.All))
        assertEquals("Installed", sectionTitle(CatalogueSection.Kind.REST, CatalogueFilter.Installed))
        assertEquals("HMI", sectionTitle(CatalogueSection.Kind.REST, CatalogueFilter.Category("hmi")))
    }

    @Test
    fun `a row notes the agent a version waits for`() {
        val launcher = index.packages.single { it.id == "launcher" }

        assertEquals("0.3.0 needs agent 1.1.0 or later", packageStatus(launcher, car)!!.note())
        assertNull(packageStatus(launcher, car.copy(agentVersion = "1.1.0"))!!.note())
        assertNull(packageStatus(launcher, null)!!.note())

        val future = pkg("future", "Future", "hmi", version("1.0.0", ">=2.0.0"))
        assertEquals("Needs agent 2.0.0 or later", packageStatus(future, car)!!.note())
    }

    private fun waits(name: String, version: String) = Unblocked(name.lowercase(), name, version, installed = null)

    @Test
    fun `the agent's card names the agent that runs and what waits for the newer one`() {
        assertEquals("Car runs agent 1.0.2.", agentUpdateText(AgentUpdate("1.1.0", "1.0.2", emptyList()), "Car"))
        assertEquals(
            "Car runs agent 1.0.2. App Launcher 0.3.0 needs the newer one.",
            agentUpdateText(AgentUpdate("1.1.0", "1.0.2", listOf(waits("App Launcher", "0.3.0"))), "Car"),
        )
        assertEquals(
            "Car runs agent 1.0.2. App Launcher 0.3.0 and 2 more need the newer one.",
            agentUpdateText(AgentUpdate("1.1.0", "1.0.2", listOf(waits("App Launcher", "0.3.0"), waits("Log Viewer", "0.9.0"), waits("Other", "1.0.0"))), "Car"),
        )
    }

    // ---- the app's own update ----

    private val newer = release("0.2.0", 200)

    private fun withUpdate(
        ui: CatalogueUiState = state(),
        offer: AppUpdateOffer? = AppUpdateOffer.Available(newer),
        update: UpdateState = UpdateState.Idle,
        jobNeedsPhone: Boolean = false,
    ) = ui.withAppUpdate(offer, "0.1.0", update, jobNeedsPhone)

    private fun card(
        update: UpdateState = UpdateState.Idle,
        offer: AppUpdateOffer = AppUpdateOffer.Available(newer),
        jobNeedsPhone: Boolean = false,
    ) = requireNotNull(withUpdate(offer = offer, update = update, jobNeedsPhone = jobNeedsPhone).appUpdate)

    @Test
    fun `the app's own update has its card while every package is shown`() {
        assertEquals(AppUpdateView(AppUpdateOffer.Available(newer), "0.1.0", UpdateState.Idle, waitsForJob = false), withUpdate().appUpdate)
        assertNull(withUpdate(offer = null).appUpdate)

        // It gives way to a search and to a filter, as the agent's card does.
        assertNull(withUpdate(state(query = "hello")).appUpdate)
        assertNull(withUpdate(state(filter = CatalogueFilter.Updates)).appUpdate)
        assertNull(withUpdate(state(filter = CatalogueFilter.Category("hmi"))).appUpdate)

        // It needs no head unit, which the agent's card does. It does need a catalogue: the release comes from one.
        assertNotNull(withUpdate(state(head = null)).appUpdate)
        assertNull(withUpdate(CatalogueUiState()).appUpdate)

        // The agent's card is still there beneath it.
        assertNotNull(withUpdate().agentUpdate)
    }

    @Test
    fun `the app's card says where the update stands, and has its button only when one can begin`() {
        assertNull(appUpdateStatus(card()))
        assertEquals(AppUpdateAction.UPDATE, appUpdateAction(card()))

        assertEquals("Downloading…", appUpdateStatus(card(UpdateState.Downloading(10, 100))))
        assertTrue(appUpdateStatus(card(UpdateState.Installing))!!.startsWith("Installing."))
        assertEquals("Android is asking whether to install the update.", appUpdateStatus(card(UpdateState.Confirming)))
        for (busy in listOf(UpdateState.Downloading(10, 100), UpdateState.Installing, UpdateState.Confirming)) {
            assertNull(appUpdateAction(card(busy)))
        }

        // Until Android lets the app install apps, the button leads to where that is allowed.
        val unallowed = card(UpdateState.NeedsPermission)
        assertTrue(appUpdateStatus(unallowed)!!.startsWith("Android has to be told, once,"))
        assertEquals(AppUpdateAction.ALLOW, appUpdateAction(unallowed))

        // A failure gives its reason, and the button is back for another try.
        val failed = card(UpdateState.Failed("The update was cancelled."))
        assertEquals("The update was cancelled.", appUpdateStatus(failed))
        assertEquals(AppUpdateAction.RETRY, appUpdateAction(failed))
    }

    @Test
    fun `while a job needs the phone the app's card says why the update waits`() {
        val waiting = card(jobNeedsPhone = true)
        assertTrue(waiting.waitsForJob)
        assertEquals("A job is running on a head unit. Update the app once it has finished.", appUpdateStatus(waiting))
        // The button is there, and the screen shows it switched off.
        assertEquals(AppUpdateAction.UPDATE, appUpdateAction(waiting))
    }

    @Test
    fun `a copy that cannot take the release is told why and has no button`() {
        val otherKey = card(offer = AppUpdateOffer.Blocked(newer, AppUpdateOffer.Reason.OTHER_KEY))
        assertTrue(appUpdateStatus(otherKey)!!.startsWith("This copy can't update itself"))
        assertNull(appUpdateAction(otherKey))

        val oldAndroid = card(offer = AppUpdateOffer.Blocked(newer, AppUpdateOffer.Reason.ANDROID_TOO_OLD))
        assertEquals("It needs a newer version of Android than this phone has.", appUpdateStatus(oldAndroid))
        assertNull(appUpdateAction(oldAndroid))
    }
}

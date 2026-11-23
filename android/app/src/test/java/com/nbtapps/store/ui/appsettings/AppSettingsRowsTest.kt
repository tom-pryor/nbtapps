// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.appsettings

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.update.RUNNING
import com.nbtapps.store.data.whatsnew.VersionNotes
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.ui.catalogue.longReleaseDate
import com.nbtapps.store.ui.catalogue.whatsNewByline
import com.nbtapps.store.ui.catalogue.whatsNewTitle
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

/**
 * The words of the app's settings and of the pages opened from them: the rows, what they say and where they go, and
 * their view model; the title of what is new and the line under it, with the release date in long form, as the sheet
 * and the page show them, and what the page says with no notes; and the line over a licence's text that names the
 * projects under it.
 */
class AppSettingsRowsTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    private val notes = VersionNotes(900, "0.9.0", "2026-11-23", listOf("A terminal on the head unit"))

    private fun state(version: String = "0.9.0", notes: VersionNotes? = this.notes, noHeadUnit: Boolean = false) =
        AppSettingsUiState(version, notes, noHeadUnit)

    @Test
    fun `the version row gives the version name only, and opens nothing`() {
        assertEquals(AppSettingsRow("Version", "0.9.0"), aboutRows(state()).first())
        assertEquals(AppSettingsRow("Version", "0.9.0-trial"), aboutRows(state(version = "0.9.0-trial")).first())
    }

    @Test
    fun `what's new opens the page of the notes where the phone has them, and the changelog where it has none`() {
        assertEquals(
            AppSettingsRow("What's new", "The notes of version 0.9.0", AppSettingsTarget.WhatsNew),
            aboutRows(state()).last(),
        )
        assertEquals(
            AppSettingsRow(
                "What's new",
                "The changelog on GitHub",
                AppSettingsTarget.WebPage("https://github.com/tom-pryor/nbtapps/blob/main/CHANGELOG.md"),
            ),
            aboutRows(state(notes = null)).last(),
        )
    }

    @Test
    fun `the set-up row is for a first car with no head unit saved, and for another car otherwise`() {
        assertEquals(
            AppSettingsRow("Set up your car", "Add your head unit and pair it", AppSettingsTarget.SetUp),
            moreRows(state(noHeadUnit = true)).first(),
        )
        assertEquals(
            AppSettingsRow("Set up another car", "Add another head unit and pair it", AppSettingsTarget.AddCar),
            moreRows(state(noHeadUnit = false)).first(),
        )
    }

    @Test
    fun `the licences and the app's page on GitHub follow`() {
        assertEquals(
            listOf(
                AppSettingsRow("Open-source licences", "The code this app includes", AppSettingsTarget.Licences),
                AppSettingsRow("NBTApps on GitHub", "Source, issues and releases", AppSettingsTarget.WebPage("https://github.com/tom-pryor/nbtapps")),
            ),
            moreRows(state()).drop(1),
        )
    }

    @Test
    fun `the title and the line over the notes, with the release date where there is one`() {
        assertEquals("What's new in 0.9.0", whatsNewTitle(notes))
        assertEquals("NBTApps 0.9.0 · 23 November 2026", whatsNewByline("NBTApps", notes))
        assertEquals("NBTApps 0.9.0", whatsNewByline("NBTApps", notes.copy(released = null)))
        assertEquals("NBTApps 0.9.0", whatsNewByline("NBTApps", notes.copy(released = "")))
        assertEquals("NBTApps 0.9.0", whatsNewByline("NBTApps", notes.copy(released = " ")))
        assertEquals("The notes of version 0.9.0-trial aren't on this phone yet.", noNotesText("0.9.0-trial"))
    }

    @Test
    fun `a plain release date is written out in English, and anything else is shown as it comes`() {
        assertEquals("23 November 2026", longReleaseDate("2026-11-23"))
        assertEquals("5 January 2027", longReleaseDate("2027-01-05"))
        assertEquals("NBTApps 0.9.0 · 5 January 2027", whatsNewByline("NBTApps", notes.copy(released = "2027-01-05")))
        assertEquals("November 2026", longReleaseDate("November 2026"))
        assertEquals("2026-11-31", longReleaseDate("2026-11-31"))
        assertEquals("2026-11-23T10:00:00Z", longReleaseDate("2026-11-23T10:00:00Z"))
        assertEquals("NBTApps 0.9.0 · soon", whatsNewByline("NBTApps", notes.copy(released = "soon")))
    }

    @Test
    fun `the view model gives the version, the notes and whether a head unit is saved`() = runTest {
        val headUnits = InMemoryHeadUnitRepository()
        val notesFlow = MutableStateFlow<VersionNotes?>(null)
        val viewModel = mainDispatcherRule.track(AppSettingsViewModel(RUNNING, headUnits, notesFlow))
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }

        assertEquals(AppSettingsUiState("0.1.0", notes = null, noHeadUnit = true), viewModel.uiState.value)
        notesFlow.value = notes
        headUnits.add(HeadUnit("car", "Car", listOf(TcpEndpoint("192.168.0.1", 7144))))
        assertEquals(AppSettingsUiState("0.1.0", notes, noHeadUnit = false), viewModel.uiState.value)
    }

    @Test
    fun `a licence's line names the projects under it`() {
        assertEquals("The licence of Okio.", licenceUsedBy(listOf("Okio")))
        assertEquals("The licence of AndroidX, Okio and JSpecify.", licenceUsedBy(listOf("AndroidX", "Okio", "JSpecify")))
        assertEquals("", licenceUsedBy(emptyList()))
    }
}

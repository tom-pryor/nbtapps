// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.appsettings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.data.whatsnew.VersionNotes
import com.nbtapps.store.data.whatsnew.WhatsNewRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn

/** The address of the app's source, its issues and its releases. */
const val GITHUB_URL = "https://github.com/tom-pryor/nbtapps"

/** The changelog of every version, for where the phone has no notes of the version that runs. */
const val CHANGELOG_URL = "https://github.com/tom-pryor/nbtapps/blob/main/CHANGELOG.md"

data class AppSettingsUiState(
    /** The version name of the copy that runs, such as "0.9.0" or "0.9.0-trial". */
    val version: String,
    /** The notes of the version that runs, where the phone has them, seen or not; null where it has none. */
    val notes: VersionNotes?,
    /** No head unit is saved: the set-up is offered as for a first car. */
    val noHeadUnit: Boolean,
)

/** Where a row of the app's settings goes. */
sealed interface AppSettingsTarget {
    /** The notes of the version that runs, as a page. */
    data object WhatsNew : AppSettingsTarget

    /** The set-up, for the first car. */
    data object SetUp : AppSettingsTarget

    /** The set-up, for a further car. */
    data object AddCar : AppSettingsTarget

    /** The projects whose code the app includes, with their licences. */
    data object Licences : AppSettingsTarget

    /** A page in the phone's browser. */
    data class WebPage(val url: String) : AppSettingsTarget
}

/** A row of the app's settings: [target] is null for a row that only says something. */
data class AppSettingsRow(val title: String, val detail: String, val target: AppSettingsTarget? = null)

/**
 * The rows under "About": the version, and what is new in it. With no notes of it on the phone, the second opens the
 * changelog on GitHub in their place.
 */
internal fun aboutRows(state: AppSettingsUiState): List<AppSettingsRow> = listOf(
    AppSettingsRow("Version", state.version),
    state.notes?.let { AppSettingsRow("What's new", "The notes of version ${it.versionName}", AppSettingsTarget.WhatsNew) }
        ?: AppSettingsRow("What's new", "The changelog on GitHub", AppSettingsTarget.WebPage(CHANGELOG_URL)),
)

/** The rows under "More": the set-up, for a first car or a further one, the licences and the app's source. */
internal fun moreRows(state: AppSettingsUiState): List<AppSettingsRow> = listOf(
    if (state.noHeadUnit) {
        AppSettingsRow("Set up your car", "Add your head unit and pair it", AppSettingsTarget.SetUp)
    } else {
        AppSettingsRow("Set up another car", "Add another head unit and pair it", AppSettingsTarget.AddCar)
    },
    AppSettingsRow("Open-source licences", "The code this app includes", AppSettingsTarget.Licences),
    AppSettingsRow("NBTApps on GitHub", "Source, issues and releases", AppSettingsTarget.WebPage(GITHUB_URL)),
)

/** The app's own settings, and the page of what is new in the version that runs, which is opened from them. */
@HiltViewModel
class AppSettingsViewModel internal constructor(
    installed: InstalledApp,
    headUnits: HeadUnitRepository,
    /** [WhatsNewRepository.notes]. */
    notes: Flow<VersionNotes?>,
) : ViewModel() {

    @Inject
    constructor(installed: InstalledApp, headUnits: HeadUnitRepository, whatsNew: WhatsNewRepository) :
        this(installed, headUnits, whatsNew.notes)

    /** Null until the saved head units and the notes have been read. */
    val uiState: StateFlow<AppSettingsUiState?> =
        combine(notes, headUnits.headUnits) { versionNotes, saved ->
            AppSettingsUiState(installed.versionName, versionNotes, noHeadUnit = saved.isEmpty())
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), null)
}

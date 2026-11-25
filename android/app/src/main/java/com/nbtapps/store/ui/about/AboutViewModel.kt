// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.about

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.data.whatsnew.VersionNotes
import com.nbtapps.store.data.whatsnew.WhatsNewRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn

/** The address of the app's source, its issues and its releases. */
const val GITHUB_URL = "https://github.com/tom-pryor/nbtapps"

/** The changelog of every version, for where the phone has no notes of the version that runs. */
const val CHANGELOG_URL = "https://github.com/tom-pryor/nbtapps/blob/main/CHANGELOG.md"

data class AboutUiState(
    /** The version name of the copy that runs, such as "0.9.0" or "0.9.0-trial". */
    val version: String,
    /** The notes of the version that runs, where the phone has them, seen or not; null where it has none. */
    val notes: VersionNotes?,
)

/** Where a row of the About page goes. */
sealed interface AboutTarget {
    /** The notes of the version that runs, as a page. */
    data object WhatsNew : AboutTarget

    /** The projects whose code the app includes, with their licences. */
    data object Licences : AboutTarget

    /** A page in the phone's browser. */
    data class WebPage(val url: String) : AboutTarget
}

/** A row of the About page: [target] is null for a row that only says something. */
data class AboutRow(val title: String, val detail: String, val target: AboutTarget? = null)

/**
 * The rows under "About NBTApps": the version, and what is new in it. With no notes of it on the phone, the second
 * opens the changelog on GitHub in their place.
 */
internal fun aboutRows(state: AboutUiState): List<AboutRow> = listOf(
    AboutRow("Version", state.version),
    state.notes?.let { AboutRow("What's new", "The notes of version ${it.versionName}", AboutTarget.WhatsNew) }
        ?: AboutRow("What's new", "The changelog on GitHub", AboutTarget.WebPage(CHANGELOG_URL)),
)

/** The rows under "More": the licences and the app's source. */
internal val moreRows: List<AboutRow> = listOf(
    AboutRow("Open-source licences", "The code this app includes", AboutTarget.Licences),
    AboutRow("NBTApps on GitHub", "Source, issues and releases", AboutTarget.WebPage(GITHUB_URL)),
)

/** The About page, and the page of what is new in the version that runs, which is opened from it. */
@HiltViewModel
class AboutViewModel internal constructor(
    installed: InstalledApp,
    /** [WhatsNewRepository.notes]. */
    notes: Flow<VersionNotes?>,
) : ViewModel() {

    @Inject
    constructor(installed: InstalledApp, whatsNew: WhatsNewRepository) : this(installed, whatsNew.notes)

    /** Null until the notes have been read. */
    val uiState: StateFlow<AboutUiState?> =
        notes.map { AboutUiState(installed.versionName, it) }
            .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), null)
}

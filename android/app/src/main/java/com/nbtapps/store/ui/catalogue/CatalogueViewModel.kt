// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PrereleaseRepository
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.AgentUpdate
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.PackageStatus
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.agentUpdate
import com.nbtapps.store.data.jobs.holdings
import com.nbtapps.store.data.jobs.packageStatus
import com.nbtapps.store.data.ssh.SshPort
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.data.update.AppUpdateOffer
import com.nbtapps.store.data.update.AppUpdater
import com.nbtapps.store.data.update.InstalledApp
import com.nbtapps.store.data.update.UpdateState
import com.nbtapps.store.data.update.appUpdate
import com.nbtapps.store.model.CataloguePackage
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** Which of the catalogue's packages the list shows. */
sealed interface CatalogueFilter {
    data object All : CatalogueFilter

    /** Those the head unit has. Offered only when a head unit's list is known. */
    data object Installed : CatalogueFilter

    /** Those the head unit has an older version of than it could have. Offered as [Installed] is. */
    data object Updates : CatalogueFilter

    /** Those of one of the catalogue's categories, by its id as published. */
    data class Category(val id: String) : CatalogueFilter
}

/** One package in the list, and where it stands on the head unit. [status] is null for a package with no version. */
data class CatalogueRow(val pkg: CataloguePackage, val status: PackageStatus?)

/** A run of rows under one heading. */
data class CatalogueSection(val kind: Kind, val rows: List<CatalogueRow>) {
    enum class Kind {
        /** The whole list, with no heading: there is nothing to set apart, or a search is shown. */
        PLAIN,

        /** The packages with an update, ahead of the rest. */
        UPDATES,

        /** What follows the updates. */
        REST,
    }
}

/** A newer release of the app itself, and where updating to it stands. */
data class AppUpdateView(
    val offer: AppUpdateOffer,
    /** The version of the copy that runs. */
    val installedVersion: String,
    val state: UpdateState,
    /** What an update would end with the app, so that it waits; null where nothing does. */
    val waitsFor: UpdateWait? = null,
)

/** What the app's update waits for: the update would end it with the app. */
enum class UpdateWait {
    /** A job on a head unit that needs the phone. */
    JOB,

    /** A terminal open, or being opened, on a head unit. */
    TERMINAL,

    /** The SSH port for other apps on, or being switched on. */
    PORT,
}

/** What the update waits for, the job first: see [UpdateWait]. Null where nothing is in the way. */
internal fun updateWait(jobNeedsPhone: Boolean, terminalActive: Boolean, portActive: Boolean): UpdateWait? = when {
    jobNeedsPhone -> UpdateWait.JOB
    terminalActive -> UpdateWait.TERMINAL
    portActive -> UpdateWait.PORT
    else -> null
}

data class CatalogueUiState(
    /** False until an index has been accepted, so the screen can tell "loading" from "empty". */
    val loaded: Boolean = false,
    /** The catalogue is being fetched again, or the head unit's list is being read again, or both. */
    val refreshing: Boolean = false,
    val error: String? = null,
    val sourceName: String? = null,
    val query: String = "",
    val categories: List<String> = emptyList(),
    val filter: CatalogueFilter = CatalogueFilter.All,
    /**
     * The head unit the list is shown against, and what it holds. Null when nothing is known of one: the list
     * is then the catalogue alone, and [CatalogueFilter.Installed] and [CatalogueFilter.Updates] are not offered.
     */
    val headUnit: Holdings? = null,
    /** How many of the catalogue's packages have an update for the head unit, whatever the list is narrowed to. */
    val updateCount: Int = 0,
    val sections: List<CatalogueSection> = emptyList(),
    /** A newer agent for the head unit, when the list is the place to say so: with every package shown. */
    val agentUpdate: AgentUpdate? = null,
    /** A newer release of the app itself, shown where the agent's update is: with every package shown. */
    val appUpdate: AppUpdateView? = null,
    /** The head unit's list is being read again over the link, along with the catalogue. */
    val refreshingHeadUnit: Boolean = false,
    /** Why the head unit's list could not be read again, while the link it was asked over is still there. */
    val headUnitError: String? = null,
) {
    /** The packages shown, in the order shown. */
    val packages: List<CataloguePackage> get() = sections.flatMap { it.rows }.map { it.pkg }
}

/** The reading of the head unit's list that a pull on the catalogue starts. */
internal data class HeadUnitRefresh(val running: Boolean = false, val error: String? = null)

/**
 * What the catalogue screen shows for the [catalogue] as it is held, against the head unit of [head].
 *
 * Packages with an update come first, under their own heading, and the rest by name; a search shows one list by
 * name. A filter that no longer applies falls back to everything: a category the catalogue has dropped, or one
 * of the head unit's two with no head unit known.
 *
 * While the agent's update has its card ([CatalogueUiState.agentUpdate]) the agent's package is not also among
 * the updates at the top; it stays in the list, and counts as an update.
 *
 * Each package is offered its pre-release versions where it is among [switchedOn], and only its stable ones
 * otherwise: see [offeredVersions][com.nbtapps.store.data.jobs.offeredVersions]. A version that waits for another
 * package's pre-release to be switched on is no update, here as on its page: its row notes what it waits for.
 */
internal fun catalogueUiState(
    catalogue: CatalogueState,
    head: Holdings?,
    query: String,
    selected: CatalogueFilter,
    refresh: HeadUnitRefresh,
    switchedOn: Set<String> = emptySet(),
): CatalogueUiState {
    val index = catalogue.index
    // Each row's status is the one its package's page shows, what it waits for in other packages among it.
    val all = index?.let { loaded -> loaded.packages.map { CatalogueRow(it, packageStatus(loaded, it, head, switchedOn)) } }.orEmpty()
    val categories = all.map { it.pkg.category }.filter { it.isNotEmpty() }.distinct().sorted()
    val filter = when (selected) {
        CatalogueFilter.All -> selected
        CatalogueFilter.Installed, CatalogueFilter.Updates -> selected.takeIf { head != null }
        is CatalogueFilter.Category -> selected.takeIf { it.id in categories }
    } ?: CatalogueFilter.All
    val searching = query.isNotBlank()

    val shown = all
        .filter { row ->
            when (filter) {
                CatalogueFilter.All -> true
                CatalogueFilter.Installed -> row.status?.installed != null
                CatalogueFilter.Updates -> row.status?.updateAvailable == true
                is CatalogueFilter.Category -> row.pkg.category == filter.id
            }
        }
        .filter { it.pkg.matches(query) }
        .sortedBy { it.pkg.name.lowercase() }

    val card = index?.takeIf { filter == CatalogueFilter.All && !searching }?.let { agentUpdate(it, head, switchedOn) }
    val updates = shown.filter { it.status?.updateAvailable == true && !(card != null && it.pkg.id == AGENT_PACKAGE_ID) }
    val sections = when {
        shown.isEmpty() -> emptyList()
        searching || updates.isEmpty() -> listOf(CatalogueSection(CatalogueSection.Kind.PLAIN, shown))
        else -> listOf(
            CatalogueSection(CatalogueSection.Kind.UPDATES, updates),
            CatalogueSection(CatalogueSection.Kind.REST, shown - updates.toSet()),
        ).filter { it.rows.isNotEmpty() }
    }

    val live = head?.live == true
    return CatalogueUiState(
        loaded = index != null,
        refreshing = catalogue.refreshing || refresh.running,
        error = catalogue.error,
        sourceName = index?.source?.name,
        query = query,
        categories = categories,
        filter = filter,
        headUnit = head,
        updateCount = all.count { it.status?.updateAvailable == true },
        sections = sections,
        agentUpdate = card,
        refreshingHeadUnit = refresh.running && live,
        headUnitError = refresh.error.takeIf { live },
    )
}

/**
 * The list with the app's own update put to it. Its card stands above the agent's, under the same rule: it is
 * there while every package is shown, and gives way to a search or a filter. An update that is under way goes on
 * out of sight meanwhile.
 */
internal fun CatalogueUiState.withAppUpdate(
    offer: AppUpdateOffer?,
    installedVersion: String,
    state: UpdateState,
    waitsFor: UpdateWait?,
): CatalogueUiState = copy(
    appUpdate = offer
        ?.takeIf { loaded && filter == CatalogueFilter.All && query.isBlank() }
        ?.let { AppUpdateView(it, installedVersion, state, waitsFor) },
)

private fun CataloguePackage.matches(query: String): Boolean {
    val words = query.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
    val haystack = listOf(name, summary, author, id)
    return words.all { word -> haystack.any { it.contains(word, ignoreCase = true) } }
}

@HiltViewModel
class CatalogueViewModel internal constructor(
    private val repository: CatalogueRepository,
    headUnits: HeadUnitRepository,
    private val jobs: JobRepository,
    private val updater: AppUpdater,
    private val installed: InstalledApp,
    prereleases: PrereleaseRepository,
    /** What else an update would end with the app: a terminal or the SSH port for other apps ([UpdateWait]). */
    private val inUse: Flow<UpdateWait?> = flowOf(null),
) : ViewModel() {

    @Inject
    constructor(
        repository: CatalogueRepository,
        headUnits: HeadUnitRepository,
        jobs: JobRepository,
        updater: AppUpdater,
        installed: InstalledApp,
        prereleases: PrereleaseRepository,
        terminal: TerminalRepository,
        port: SshPort,
    ) : this(
        repository, headUnits, jobs, updater, installed, prereleases,
        combine(terminal.state, port.state) { shell, ssh -> updateWait(false, shell.active, ssh.active) },
    )

    private val query = MutableStateFlow("")
    private val filter = MutableStateFlow<CatalogueFilter>(CatalogueFilter.All)
    private val headUnitRefresh = MutableStateFlow(HeadUnitRefresh())

    /** The head unit the list is shown against, and the packages switched on for pre-releases. */
    private val head = combine(combine(headUnits.headUnits, jobs.connection, ::holdings), prereleases.switchedOn, ::Pair)

    /**
     * The app's own update: what the catalogue offers this copy, how far it has got, and what is in its way: a job, a
     * terminal or the SSH port.
     */
    private val ownUpdate = combine(repository.catalogue, updater.state, jobs.job, inUse) { catalogue, state, job, other ->
        Triple(appUpdate(catalogue.index?.app, installed), state, if (job?.needsPhone == true) UpdateWait.JOB else other)
    }

    /** No head unit is saved, so the screen offers the set-up. False until the saved head units have been read. */
    val noHeadUnit: StateFlow<Boolean> =
        headUnits.headUnits.map { it.isEmpty() }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), false)

    val uiState: StateFlow<CatalogueUiState> =
        combine(repository.catalogue, head, query, filter, headUnitRefresh) { catalogue, (head, switchedOn), query, filter, refresh ->
            catalogueUiState(catalogue, head, query, filter, refresh, switchedOn)
        }
            .combine(ownUpdate) { ui, (offer, state, busy) -> ui.withAppUpdate(offer, installed.versionName, state, busy) }
            .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), CatalogueUiState())

    init {
        // The head unit's list was read when the link was opened; only the catalogue is still to come.
        if (repository.catalogue.value.index == null) viewModelScope.launch { repository.refresh() }
    }

    /**
     * Fetches the catalogue again and, over a link that is open, reads the head unit's profile and installed
     * list again: one pull brings both sides of the list up to date. Another phone, a USB stick or the car's own
     * screen may have changed what is installed, and the agent does not say so by itself.
     */
    fun refresh() {
        viewModelScope.launch {
            val connected = jobs.connection.value is ConnectionState.Connected
            headUnitRefresh.value = HeadUnitRefresh(running = connected)
            coroutineScope {
                launch { repository.refresh() }
                if (connected) {
                    launch {
                        val failure = jobs.refresh().exceptionOrNull()
                        headUnitRefresh.value = HeadUnitRefresh(error = failure?.let { it.message ?: it.javaClass.simpleName })
                    }
                }
            }
        }
    }

    /** Updates the app to the release the catalogue offers, if this copy can take it. [AppUpdater] refuses while a job runs. */
    fun updateApp() {
        val offer = uiState.value.appUpdate?.offer as? AppUpdateOffer.Available ?: return
        updater.start(offer.release)
    }

    fun setQuery(text: String) {
        query.value = text
    }

    fun selectFilter(selected: CatalogueFilter) {
        filter.value = selected
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PrereleaseRepository
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.PrereleaseAsk
import com.nbtapps.store.data.jobs.Resolver
import com.nbtapps.store.data.jobs.holdings
import com.nbtapps.store.data.jobs.offeredVersions
import com.nbtapps.store.data.jobs.resolvedStatus
import com.nbtapps.store.data.packages.DownloadState
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.AppVersion
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.ChangelogEntry
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.SemVer
import com.nbtapps.store.ui.navigation.CataloguePackageRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import java.net.URI
import java.net.URISyntaxException
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

sealed interface DetailsState {
    data object Loading : DetailsState
    data class Loaded(val details: PackageDetails) : DetailsState
    data class Failed(val reason: String) : DetailsState
}

/** One dependency of the latest version, with the catalogue's name for it when the catalogue has it. */
data class DependencyRow(
    val id: String,
    val name: String,
    val versionRange: String?,
    val inCatalogue: Boolean,
)

/** Where the package stands between the catalogue, the phone and the head unit the app is working with. */
data class GetState(
    /** The version on offer: the newest that suits that head unit, or the newest of all. */
    val version: PackageVersion,
    /** Whether that version's archive, verified, is on the phone. */
    val stored: Boolean = false,
    /**
     * That archive's manifest uses features this version of the app does not know
     * ([StoredPackage.laterFormat]): the page says so while it is on the phone.
     */
    val laterFormat: Boolean = false,
    /** The archives still to download before it can be installed: this one and the dependencies it needs. */
    val toDownload: List<Resolver.Needed> = emptyList(),
    val downloading: DownloadState.Running? = null,
    val downloadError: String? = null,
    /** The name of the head unit the app is working with, or null when nothing is known of one. */
    val headUnitName: String? = null,
    /** Whether the app has a link to that head unit, which an install needs. */
    val connected: Boolean = false,
    /** The version on that head unit, if the package is installed there. */
    val installedVersion: String? = null,
    /**
     * When [installedVersion] was read, in milliseconds since the epoch, if that was not over a link that is open
     * now: it is then what the head unit said the last time the app was connected to it.
     */
    val installedAsOf: Long? = null,
    /** Why the package cannot go on that head unit, if it cannot. */
    val incompatible: String? = null,
    /**
     * A version newer than [version] that only a newer version of this app may install, where the head unit's agent
     * runs it ([PackageStatus.heldBackByApp][com.nbtapps.store.data.jobs.PackageStatus.heldBackByApp]).
     */
    val needsNewerApp: String? = null,
    /** [version] is a pre-release. */
    val offeredPrerelease: Boolean = false,
    /** [installedVersion] is a pre-release. */
    val installedPrerelease: Boolean = false,
    /**
     * The package is out only in pre-release versions and its switch is off: nothing is downloaded or installed
     * until the user turns it on, and Install asks for that first.
     */
    val needsSwitch: Boolean = false,
    /** The packages whose pre-releases [version] needs, with their switches off: dependencies first, then the agent. */
    val asks: List<PrereleaseAsk> = emptyList(),
    /**
     * The head unit has [version] or a newer one, as the package's status says: see
     * [PackageStatus.upToDate][com.nbtapps.store.data.jobs.PackageStatus.upToDate].
     */
    val upToDate: Boolean = false,
    /** This app's id of that head unit, for its screens. Null when nothing is known of one. */
    val headUnitId: String? = null,
    /**
     * The installed package has settings, as the head unit said over the link that is open or the last time there was
     * one. Its settings are changed over a link to that head unit only ([connected]).
     */
    val settings: Boolean = false,
    /**
     * A job that needs the phone is running ([JobView.needsPhone][com.nbtapps.store.data.jobs.JobView.needsPhone]),
     * on this head unit or another: nothing else is started on a head unit until it has finished.
     */
    val busy: Boolean = false,
    /**
     * The head unit has an older version than [version] and may take it: see
     * [PackageStatus.updateAvailable][com.nbtapps.store.data.jobs.PackageStatus.updateAvailable], as the list's row has it.
     */
    val updateAvailable: Boolean = false,
    /** Whether the package can be uninstalled from its page, and if not, why. */
    val uninstall: UninstallState = UninstallState.None,
) {
    /** Everything is in place for Install or Update but a job that is running. */
    val readyToInstall: Boolean
        get() = stored && toDownload.isEmpty() && connected && incompatible == null && asks.isEmpty() && !needsSwitch && !upToDate

    val canInstall: Boolean get() = readyToInstall && !busy

    /**
     * The download and what is said of the archive on the phone are shown: not where the head unit is up to date,
     * since nothing would be installed from them.
     */
    val showsDownload: Boolean get() = !upToDate
}

/** Whether the package's page offers to uninstall it from the head unit. */
sealed interface UninstallState {
    /** It is not on the head unit: there is nothing to uninstall, and nothing is said of it. */
    data object None : UninstallState

    /** The Uninstall button is there and may be pressed. */
    data object Ready : UninstallState

    /** It is installed, and Uninstall cannot be pressed: see [Reason]. */
    data class Blocked(val reason: Reason) : UninstallState

    enum class Reason {
        /** The agent: everything else on the head unit needs it, so the page has no button for it. */
        AGENT,

        /** The app has no link to the head unit: the button is there, greyed. */
        NO_LINK,

        /** A job that needs the phone is running: the button is there, greyed. */
        BUSY,
    }
}

/** What the page says of uninstalling a package that is [installed], in the order the rules apply. */
internal fun uninstallState(id: String, installed: Boolean, connected: Boolean, busy: Boolean): UninstallState = when {
    !installed -> UninstallState.None
    id == AGENT_PACKAGE_ID -> UninstallState.Blocked(UninstallState.Reason.AGENT)
    !connected -> UninstallState.Blocked(UninstallState.Reason.NO_LINK)
    busy -> UninstallState.Blocked(UninstallState.Reason.BUSY)
    else -> UninstallState.Ready
}

data class CataloguePackageUiState(
    /** True until the catalogue has loaded or failed to; [pkg] is only meaningful after that. */
    val loading: Boolean = true,
    val pkg: CataloguePackage? = null,
    val sourceName: String? = null,
    /** The newest version the page describes: of the package's [offeredVersions], or its newest pre-release where it has nothing else. */
    val newest: PackageVersion? = null,
    val dependencies: List<DependencyRow> = emptyList(),
    val details: DetailsState = DetailsState.Loading,
    val get: GetState? = null,
    /** This package's pre-release switch is on. */
    val prereleaseOn: Boolean = false,
) {
    /** The switch is shown where there is something for it to do: the package has pre-releases, or it is on already. */
    val showsPrereleaseSwitch: Boolean get() = pkg != null && (pkg.prereleases.isNotEmpty() || prereleaseOn)
}

/** One version in the page's list of them, newest first, and whether it is the one on the head unit. */
data class VersionRow(val version: PackageVersion, val prerelease: Boolean, val installed: Boolean = false)

/** Every version of [pkg], stable and pre-release, newest first, with the head unit's version, [installed], marked. */
internal fun allVersions(pkg: CataloguePackage, installed: String? = null): List<VersionRow> =
    offeredVersions(pkg, true).map { VersionRow(it, pkg.isPrerelease(it.version), installed = sameVersion(it.version, installed)) }

/**
 * One entry of what is new, whether it is a pre-release's, when its version was released where the catalogue says,
 * and whether it is the version on the head unit.
 */
data class ChangelogRow(
    val entry: ChangelogEntry,
    val prerelease: Boolean,
    val released: String? = null,
    val installed: Boolean = false,
)

/**
 * What is new in [pkg], from its [details]: the stable versions' notes as published, and where there are notes of
 * pre-releases, those among them, newest first. A pre-release's notes are marked as such, unless the version has
 * since been promoted to a stable one. Entries without notes are left out. Each has its version's release date from
 * [pkg]'s lists of versions, and the one of the version [installed] on the head unit is marked.
 */
internal fun changelog(details: PackageDetails, pkg: CataloguePackage?, installed: String? = null): List<ChangelogRow> {
    val stable = details.changelog.map { ChangelogRow(it, prerelease = SemVer.parseOrNull(it.version)?.preRelease?.isNotEmpty() == true) }
    val rows = if (details.prereleaseChangelog.isEmpty()) {
        stable
    } else {
        val pre = details.prereleaseChangelog.map { ChangelogRow(it, prerelease = pkg == null || !listedAsStable(pkg, it.version)) }
        // A stable sort: entries of equal or unreadable versions keep the order they were published in.
        (stable + pre).sortedByDescending { SemVer.parseOrNull(it.entry.version) }
    }
    return rows.filter { it.entry.notes.isNotEmpty() }.map { row ->
        row.copy(released = pkg?.let { releasedOf(it, row.entry.version) }, installed = sameVersion(row.entry.version, installed))
    }
}

/** When [version] of [pkg] was released, from its stable and pre-release versions. Null where neither says. */
private fun releasedOf(pkg: CataloguePackage, version: String): String? {
    val parsed = SemVer.parseOrNull(version) ?: return null
    return (pkg.versions + pkg.prereleases).firstOrNull { SemVer.parseOrNull(it.version)?.compareTo(parsed) == 0 }?.released
}

/** [a] and [b] are the same version, as semantic versions: never where either cannot be read as one. */
private fun sameVersion(a: String, b: String?): Boolean {
    val left = SemVer.parseOrNull(a) ?: return false
    val right = b?.let(SemVer::parseOrNull) ?: return false
    return left.compareTo(right) == 0
}

/**
 * A list on the page as it is shown: its [rows], a row added after them under a line ([extra]), the label of the
 * button that opens or closes it (none where there is nothing more to show), and the [notice] of what is left out.
 */
data class ShownList<T>(val rows: List<T>, val extra: T? = null, val toggle: String? = null, val notice: String? = null)

/** The most entries a list of the page shows when it is open: the rest are on the package's website, where it has one. */
internal const val LIST_CAP = 10

/** The entries of what is new that are shown while the list is closed. */
internal const val CHANGELOG_CLOSED = 3

private const val REST_ON_WEBSITE = " For the rest, see the package's website."

/**
 * What is new as it is shown: the newest [CHANGELOG_CLOSED] entries while closed, the newest [LIST_CAP] while
 * [open]. A package with a [website] is pointed to for the rest.
 */
internal fun shownChangelog(rows: List<ChangelogRow>, open: Boolean, website: Boolean): ShownList<ChangelogRow> {
    val count = rows.size
    if (count <= CHANGELOG_CLOSED) return ShownList(rows)
    val toggle = when {
        open -> "Show fewer"
        count <= LIST_CAP -> "Show all $count releases"
        else -> "Show $LIST_CAP of $count releases"
    }
    val notice = if (open && count > LIST_CAP) {
        "Showing the newest $LIST_CAP of $count releases." + if (website) REST_ON_WEBSITE else ""
    } else {
        null
    }
    return ShownList(rows.take(if (open) LIST_CAP else CHANGELOG_CLOSED), toggle = toggle, notice = notice)
}

/**
 * Every version as it is shown. Closed, one: the installed version where it is in the list, otherwise the newest
 * version on offer, [newest], which the page's latest version card names. A pre-release the user is not offered is
 * then not the one row, though the list has it. Where [newest] is not among the rows, the first row stands in.
 * [open], the newest [LIST_CAP], pre-releases among them, and the installed version after them where it is older
 * than those. A package with a [website] is pointed to for the rest.
 */
internal fun shownVersions(
    rows: List<VersionRow>,
    open: Boolean,
    website: Boolean,
    newest: PackageVersion? = null,
): ShownList<VersionRow> {
    val count = rows.size
    val installed = rows.indexOfFirst { it.installed }
    if (!open) {
        val offered = newest?.let { version -> rows.firstOrNull { sameVersion(it.version.version, version.version) } }
        val one = listOfNotNull(rows.getOrNull(installed) ?: offered ?: rows.firstOrNull())
        val toggle = when {
            count <= one.size -> null
            count <= LIST_CAP -> "Show all $count versions"
            else -> "Show $LIST_CAP of $count versions"
        }
        return ShownList(one, toggle = toggle)
    }
    val extra = if (installed >= LIST_CAP) rows[installed] else null
    val notice = if (count > LIST_CAP) {
        "Showing the newest $LIST_CAP of $count versions" + (if (extra != null) ", and the installed one." else ".") +
            if (website) REST_ON_WEBSITE else ""
    } else {
        null
    }
    return ShownList(rows.take(LIST_CAP), extra, toggle = "Show fewer".takeIf { count > 1 }, notice = notice)
}

/**
 * The package's website from its [homepage], where that is an https address with a host: nothing else is opened
 * from the page.
 */
internal fun website(homepage: String?): String? {
    val address = homepage?.trim()?.takeIf { it.isNotEmpty() } ?: return null
    val uri = try {
        URI(address)
    } catch (e: URISyntaxException) {
        return null
    }
    return address.takeIf { uri.scheme.equals("https", ignoreCase = true) && !uri.host.isNullOrEmpty() }
}

/** What is said under the package's name of the head unit's copy: that it is installed, and an update that waits. */
data class HeaderMarks(val installed: String? = null, val update: String? = null)

internal fun headerMarks(get: GetState?): HeaderMarks = HeaderMarks(
    installed = get?.installedVersion?.let { "Installed $it" },
    update = get?.takeIf { it.updateAvailable }?.let { "Update to ${it.version.version}" },
)

/** [version] is among the stable versions of [pkg], and has no pre-release part: one that has been promoted. */
private fun listedAsStable(pkg: CataloguePackage, version: String): Boolean {
    val parsed = SemVer.parseOrNull(version) ?: return false
    return parsed.preRelease.isEmpty() && pkg.versions.any { SemVer.parseOrNull(it.version)?.compareTo(parsed) == 0 }
}

/**
 * What the page of the package [packageId] shows, from the [catalogue], its [details], what the phone has
 * ([stored], [downloads]), the head unit of [head], and the packages [switchedOn] for pre-releases.
 */
internal fun cataloguePackageUiState(
    catalogue: CatalogueState,
    packageId: String,
    details: DetailsState,
    stored: List<StoredPackage>,
    downloads: Map<String, DownloadState>,
    head: Holdings?,
    switchedOn: Set<String>,
    busy: Boolean = false,
): CataloguePackageUiState {
    val index = catalogue.index
    val pkg = index?.packages?.find { it.id == packageId }
    val newest = pkg?.let { offeredVersions(it, switchedOn).firstOrNull() ?: it.prereleases.firstOrNull() }
    return CataloguePackageUiState(
        loading = index == null && catalogue.error == null,
        pkg = pkg,
        sourceName = index?.source?.name,
        newest = newest,
        dependencies = newest?.depends.orEmpty().map { dependency ->
            val target = index?.packages?.find { it.id == dependency.id }
            DependencyRow(dependency.id, target?.name ?: dependency.id, dependency.version, target != null)
        },
        details = details,
        get = if (index != null && pkg != null) getState(index, pkg, stored, downloads, head, switchedOn, busy) else null,
        prereleaseOn = packageId in switchedOn,
    )
}

private fun getState(
    index: CatalogueIndex,
    pkg: CataloguePackage,
    stored: List<StoredPackage>,
    downloads: Map<String, DownloadState>,
    head: Holdings?,
    switchedOn: Set<String>,
    busy: Boolean,
): GetState? {
    // The status is the list's, so that its row says what this page does. With no link, what the head unit had the
    // last time decides what is worth downloading before the car.
    val agentVersion = head?.agentVersion
    val resolved = resolvedStatus(index, pkg, head?.installed.orEmpty(), agentVersion, switchedOn) ?: return null
    val status = resolved.status
    val resolution = resolved.resolution
    val version = status.offered
    val toDownload = resolution?.needed.orEmpty().filter { needed -> stored.none { it.sha256 == needed.version.sha256 } }
    val states = toDownload.mapNotNull { downloads[it.version.sha256] }
    val running = states.filterIsInstance<DownloadState.Running>()
    val agentAsk = status.asks.any { it.id == AGENT_PACKAGE_ID }
    val archive = stored.find { it.sha256 == version.sha256 }
    return GetState(
        version = version,
        stored = archive != null,
        laterFormat = archive?.laterFormat == true,
        toDownload = toDownload,
        downloading = if (running.isEmpty()) null else DownloadState.Running(running.sumOf { it.bytes }, running.sumOf { it.total }),
        downloadError = states.filterIsInstance<DownloadState.Failed>().firstOrNull()?.reason,
        headUnitName = head?.name,
        connected = head?.live == true,
        installedVersion = status.installed,
        installedAsOf = head?.seenAt?.takeIf { !head.live && status.installed != null },
        incompatible = when {
            status.needsSwitch -> null
            // This app's own, whatever the head unit.
            status.needsApp -> "$NEEDS_NEWER_APP: ${version.requires.app?.let(::rangeLabel)}. This app is ${AppVersion.current}."
            agentVersion == null -> null
            // Asked for instead: the agent that would run it is a pre-release.
            agentAsk -> null
            !status.suits -> "Needs agent ${version.agentRange?.let(::rangeLabel)}; this head unit's agent is $agentVersion."
            resolution?.unavailable.orEmpty().isNotEmpty() ->
                "Needs ${resolution?.unavailable.orEmpty().joinToString { it.id }}, which this catalogue doesn't have in a version that fits."
            else -> null
        },
        offeredPrerelease = status.offeredPrerelease,
        installedPrerelease = status.installedPrerelease,
        needsSwitch = status.needsSwitch,
        asks = status.asks,
        upToDate = status.upToDate,
        needsNewerApp = status.heldBack?.version?.takeIf { status.heldBackByApp },
        headUnitId = head?.headUnitId,
        settings = status.installed != null && head?.installed?.firstOrNull { it.id == pkg.id }?.settings == true,
        busy = busy,
        updateAvailable = status.updateAvailable,
        uninstall = uninstallState(pkg.id, installed = status.installed != null, connected = head?.live == true, busy = busy),
    )
}

@HiltViewModel
class CataloguePackageViewModel internal constructor(
    private val packageId: String,
    private val repository: CatalogueRepository,
    private val store: PackageStore,
    private val downloader: PackageDownloader,
    jobs: JobRepository,
    headUnits: HeadUnitRepository,
    private val prereleases: PrereleaseRepository,
) : ViewModel() {

    @Inject
    constructor(
        savedStateHandle: SavedStateHandle,
        repository: CatalogueRepository,
        store: PackageStore,
        downloader: PackageDownloader,
        jobs: JobRepository,
        headUnits: HeadUnitRepository,
        prereleases: PrereleaseRepository,
    ) : this(
        savedStateHandle.toRoute<CataloguePackageRoute>().packageId, repository, store, downloader, jobs, headUnits, prereleases,
    )

    private val details = MutableStateFlow<DetailsState>(DetailsState.Loading)

    /**
     * What the phone holds, what the head unit the app is working with does, and whether a job that needs the phone
     * is running, as the app's update card tells it.
     */
    private class Sides(
        val stored: List<StoredPackage>,
        val downloads: Map<String, DownloadState>,
        val head: Holdings?,
        val busy: Boolean,
    )

    private val sides = combine(store.packages, downloader.downloads, jobs.connection, headUnits.headUnits, jobs.job) { stored, downloads, connection, units, job ->
        Sides(stored, downloads, holdings(units, connection), busy = job?.needsPhone == true)
    }

    val uiState: StateFlow<CataloguePackageUiState> =
        combine(repository.catalogue, details, sides, prereleases.switchedOn) { catalogue, details, sides, switchedOn ->
            cataloguePackageUiState(catalogue, packageId, details, sides.stored, sides.downloads, sides.head, switchedOn, sides.busy)
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), CataloguePackageUiState())

    /** The loads of the details are numbered, from 1: the last one begun. The main thread's, as the two below are. */
    private var lastLoad = 0

    /** The load whose details are shown; 0 while none are. */
    private var shownLoad = 0

    /** The serial of the index that was held as the last load began. */
    private var loadSerial: Long? = null

    init {
        viewModelScope.launch {
            // The catalogue is only in memory, so it is gone if the process was restored onto this screen. One that is
            // held is shown at once, and fetched again beside that if it is old.
            if (repository.catalogue.value.index == null) repository.refresh() else launch { repository.refreshIfStale() }
            loadDetails()
            // A newer index may offer another version, whose changelog is in newer details.
            repository.catalogue.collect { state ->
                val serial = state.index?.serial ?: return@collect
                if (loadSerial.let { it == null || serial > it }) loadDetails()
            }
        }
    }

    fun retryDetails() = loadDetails()

    /** Downloads the offered version and the dependencies it needs that are not on the phone yet. */
    fun download() {
        uiState.value.get?.toDownload?.forEach { downloader.download(it.pkg.id, it.version) }
    }

    /**
     * Turns pre-release versions of the package [id] on or off: this package's, or one whose pre-release this one
     * needs. The screen has asked the user before turning one on.
     */
    fun setPrerelease(id: String, on: Boolean) {
        viewModelScope.launch { prereleases.set(id, on) }
    }

    /**
     * Loads the details, again if they are there. Details that are shown stay until newer ones are there, and stay
     * where there come none. Loads may overlap: an older one's answer never takes the place of a newer one's.
     */
    private fun loadDetails() {
        val load = ++lastLoad
        loadSerial = repository.catalogue.value.index?.serial
        if (details.value !is DetailsState.Loaded) details.value = DetailsState.Loading
        viewModelScope.launch {
            repository.details(packageId).fold(
                onSuccess = {
                    if (load > shownLoad) {
                        shownLoad = load
                        details.value = DetailsState.Loaded(it)
                    }
                },
                onFailure = {
                    // Where a newer load has begun, that one says how it stands; an older one may still bring details.
                    if (load == lastLoad && details.value !is DetailsState.Loaded) {
                        details.value = DetailsState.Failed(it.message ?: it.javaClass.simpleName)
                    }
                },
            )
        }
    }
}

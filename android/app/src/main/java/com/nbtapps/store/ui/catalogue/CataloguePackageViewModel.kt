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
import com.nbtapps.store.data.jobs.Resolver
import com.nbtapps.store.data.jobs.agentPrereleaseFor
import com.nbtapps.store.data.jobs.holdings
import com.nbtapps.store.data.jobs.offeredVersions
import com.nbtapps.store.data.jobs.packageStatus
import com.nbtapps.store.data.packages.DownloadState
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.ChangelogEntry
import com.nbtapps.store.model.PackageDetails
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.SemVer
import com.nbtapps.store.ui.navigation.CataloguePackageRoute
import dagger.hilt.android.lifecycle.HiltViewModel
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

/**
 * A package whose pre-release the offered version needs while its switch is off: a dependency, or the agent. The
 * page names it and offers to switch it on; nothing is switched on without the user.
 */
data class PrereleaseAsk(
    val id: String,
    /** The catalogue's name for it. */
    val name: String,
    /** The pre-release that would do. */
    val version: String,
)

/** Where the package stands between the catalogue, the phone and the head unit the app is working with. */
data class GetState(
    /** The version on offer: the newest that suits that head unit, or the newest of all. */
    val version: PackageVersion,
    /** Whether that version's archive, verified, is on the phone. */
    val stored: Boolean = false,
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
) {
    /**
     * The offered version is what the head unit already has, or older than it. With [needsSwitch], whatever the
     * head unit has: with the switch off no newer pre-release is offered.
     */
    val upToDate: Boolean
        get() {
            val installed = installedVersion?.let(SemVer::parseOrNull) ?: return false
            if (needsSwitch) return true
            val offered = SemVer.parseOrNull(version.version) ?: return false
            return installed >= offered
        }

    val canInstall: Boolean
        get() = stored && toDownload.isEmpty() && connected && incompatible == null && asks.isEmpty() && !needsSwitch && !upToDate
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

/** One version in the page's list of them, newest first. */
data class VersionRow(val version: PackageVersion, val prerelease: Boolean)

/** Every version of [pkg], stable and pre-release, newest first. */
internal fun allVersions(pkg: CataloguePackage): List<VersionRow> =
    offeredVersions(pkg, true).map { VersionRow(it, pkg.isPrerelease(it.version)) }

/** One entry of what is new, and whether it is a pre-release's. */
data class ChangelogRow(val entry: ChangelogEntry, val prerelease: Boolean)

/**
 * What is new in [pkg], from its [details]: the stable versions' notes as published, and where there are notes of
 * pre-releases, those among them, newest first. A pre-release's notes are marked as such, unless the version has
 * since been promoted to a stable one. Entries without notes are left out.
 */
internal fun changelog(details: PackageDetails, pkg: CataloguePackage?): List<ChangelogRow> {
    val stable = details.changelog.map { ChangelogRow(it, prerelease = SemVer.parseOrNull(it.version)?.preRelease?.isNotEmpty() == true) }
    val rows = if (details.prereleaseChangelog.isEmpty()) {
        stable
    } else {
        val pre = details.prereleaseChangelog.map { ChangelogRow(it, prerelease = pkg == null || !listedAsStable(pkg, it.version)) }
        // A stable sort: entries of equal or unreadable versions keep the order they were published in.
        (stable + pre).sortedByDescending { SemVer.parseOrNull(it.entry.version) }
    }
    return rows.filter { it.entry.notes.isNotEmpty() }
}

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
        get = if (index != null && pkg != null) getState(index, pkg, stored, downloads, head, switchedOn) else null,
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
): GetState? {
    val status = packageStatus(pkg, head, switchedOn) ?: return null
    val version = status.offered
    val agentVersion = head?.agentVersion
    // With no link, what the head unit had the last time decides what is worth downloading before the car. Nothing
    // is worked out for a version that waits for the package's switch.
    val resolution = Resolver.resolve(index, Resolver.Needed(pkg, version), head?.installed.orEmpty(), agentVersion, switchedOn)
        .takeIf { !status.needsSwitch }
    val toDownload = resolution?.needed.orEmpty().filter { needed -> stored.none { it.sha256 == needed.version.sha256 } }
    val states = toDownload.mapNotNull { downloads[it.version.sha256] }
    val running = states.filterIsInstance<DownloadState.Running>()
    val agentAsk = if (status.suits || status.needsSwitch) null else agentPrereleaseFor(index, version, agentVersion, switchedOn)
    val asks = resolution?.needsPrerelease.orEmpty().distinctBy { it.id }.map { need ->
        PrereleaseAsk(need.id, index.packages.find { it.id == need.id }?.name ?: need.id, need.version)
    } + listOfNotNull(
        agentAsk?.let { agent -> PrereleaseAsk(AGENT_PACKAGE_ID, index.packages.find { it.id == AGENT_PACKAGE_ID }?.name ?: AGENT_PACKAGE_ID, agent.version) },
    )
    return GetState(
        version = version,
        stored = stored.any { it.sha256 == version.sha256 },
        toDownload = toDownload,
        downloading = if (running.isEmpty()) null else DownloadState.Running(running.sumOf { it.bytes }, running.sumOf { it.total }),
        downloadError = states.filterIsInstance<DownloadState.Failed>().firstOrNull()?.reason,
        headUnitName = head?.name,
        connected = head?.live == true,
        installedVersion = status.installed,
        installedAsOf = head?.seenAt?.takeIf { !head.live && status.installed != null },
        incompatible = when {
            agentVersion == null || status.needsSwitch -> null
            // Asked for instead: the agent that would run it is a pre-release.
            agentAsk != null -> null
            !status.suits -> "Needs agent ${version.agentRange?.let(::rangeLabel)}; this head unit's agent is $agentVersion."
            resolution?.unavailable.orEmpty().isNotEmpty() ->
                "Needs ${resolution?.unavailable.orEmpty().joinToString { it.id }}, which this catalogue doesn't have in a version that fits."
            else -> null
        },
        offeredPrerelease = status.offeredPrerelease,
        installedPrerelease = status.installedPrerelease,
        needsSwitch = status.needsSwitch,
        asks = asks,
    )
}

@HiltViewModel
class CataloguePackageViewModel @Inject constructor(
    savedStateHandle: SavedStateHandle,
    private val repository: CatalogueRepository,
    private val store: PackageStore,
    private val downloader: PackageDownloader,
    jobs: JobRepository,
    headUnits: HeadUnitRepository,
    private val prereleases: PrereleaseRepository,
) : ViewModel() {

    private val packageId = savedStateHandle.toRoute<CataloguePackageRoute>().packageId

    private val details = MutableStateFlow<DetailsState>(DetailsState.Loading)

    /** What the phone holds, and what the head unit the app is working with does. */
    private class Sides(
        val stored: List<StoredPackage>,
        val downloads: Map<String, DownloadState>,
        val head: Holdings?,
    )

    private val sides = combine(store.packages, downloader.downloads, jobs.connection, headUnits.headUnits) { stored, downloads, connection, units ->
        Sides(stored, downloads, holdings(units, connection))
    }

    val uiState: StateFlow<CataloguePackageUiState> =
        combine(repository.catalogue, details, sides, prereleases.switchedOn) { catalogue, details, sides, switchedOn ->
            cataloguePackageUiState(catalogue, packageId, details, sides.stored, sides.downloads, sides.head, switchedOn)
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), CataloguePackageUiState())

    init {
        viewModelScope.launch {
            // The catalogue is only in memory, so it is gone if the process was restored onto this screen.
            if (repository.catalogue.value.index == null) repository.refresh()
            loadDetails()
        }
    }

    fun retryDetails() {
        viewModelScope.launch { loadDetails() }
    }

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

    private suspend fun loadDetails() {
        details.value = DetailsState.Loading
        details.value = repository.details(packageId).fold(
            onSuccess = { DetailsState.Loaded(it) },
            onFailure = { DetailsState.Failed(it.message ?: it.javaClass.simpleName) },
        )
    }
}

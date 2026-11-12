// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.catalogue

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.jobs.Holdings
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.Resolver
import com.nbtapps.store.data.jobs.holdings
import com.nbtapps.store.data.jobs.packageStatus
import com.nbtapps.store.data.packages.DownloadState
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
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
) {
    /** The offered version is what the head unit already has, or older than it. */
    val upToDate: Boolean
        get() {
            val installed = installedVersion?.let(SemVer::parseOrNull) ?: return false
            val offered = SemVer.parseOrNull(version.version) ?: return false
            return installed >= offered
        }

    val canInstall: Boolean get() = stored && toDownload.isEmpty() && connected && incompatible == null && !upToDate
}

data class CataloguePackageUiState(
    /** True until the catalogue has loaded or failed to; [pkg] is only meaningful after that. */
    val loading: Boolean = true,
    val pkg: CataloguePackage? = null,
    val sourceName: String? = null,
    val dependencies: List<DependencyRow> = emptyList(),
    val details: DetailsState = DetailsState.Loading,
    val get: GetState? = null,
)

@HiltViewModel
class CataloguePackageViewModel @Inject constructor(
    savedStateHandle: SavedStateHandle,
    private val repository: CatalogueRepository,
    private val store: PackageStore,
    private val downloader: PackageDownloader,
    jobs: JobRepository,
    headUnits: HeadUnitRepository,
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
        combine(repository.catalogue, details, sides) { catalogue, details, sides ->
            val index = catalogue.index
            val pkg = index?.packages?.find { it.id == packageId }
            CataloguePackageUiState(
                loading = index == null && catalogue.error == null,
                pkg = pkg,
                sourceName = index?.source?.name,
                dependencies = pkg?.latest?.depends.orEmpty().map { dependency ->
                    val target = index?.packages?.find { it.id == dependency.id }
                    DependencyRow(dependency.id, target?.name ?: dependency.id, dependency.version, target != null)
                },
                details = details,
                get = if (index != null && pkg != null) getState(index, pkg, sides) else null,
            )
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

    private fun getState(index: CatalogueIndex, pkg: CataloguePackage, sides: Sides): GetState? {
        val head = sides.head
        val status = packageStatus(pkg, head) ?: return null
        val version = status.offered
        val agentVersion = head?.agentVersion
        // With no link, what the head unit had the last time decides what is worth downloading before the car.
        val resolution = Resolver.resolve(index, Resolver.Needed(pkg, version), head?.installed.orEmpty(), agentVersion)
        val toDownload = resolution.needed.filter { needed -> sides.stored.none { it.sha256 == needed.version.sha256 } }
        val states = toDownload.mapNotNull { sides.downloads[it.version.sha256] }
        val running = states.filterIsInstance<DownloadState.Running>()
        return GetState(
            version = version,
            stored = sides.stored.any { it.sha256 == version.sha256 },
            toDownload = toDownload,
            downloading = if (running.isEmpty()) null else DownloadState.Running(running.sumOf { it.bytes }, running.sumOf { it.total }),
            downloadError = states.filterIsInstance<DownloadState.Failed>().firstOrNull()?.reason,
            headUnitName = head?.name,
            connected = head?.live == true,
            installedVersion = status.installed,
            installedAsOf = head?.seenAt?.takeIf { !head.live && status.installed != null },
            incompatible = when {
                agentVersion == null -> null
                !status.suits -> "Needs agent ${version.requires.agent?.let(::rangeLabel)}; this head unit's agent is $agentVersion."
                resolution.unavailable.isNotEmpty() ->
                    "Needs ${resolution.unavailable.joinToString { it.id }}, which this catalogue doesn't have in a version that fits."
                else -> null
            },
        )
    }

    private suspend fun loadDetails() {
        details.value = DetailsState.Loading
        details.value = repository.details(packageId).fold(
            onSuccess = { DetailsState.Loaded(it) },
            onFailure = { DetailsState.Failed(it.message ?: it.javaClass.simpleName) },
        )
    }
}

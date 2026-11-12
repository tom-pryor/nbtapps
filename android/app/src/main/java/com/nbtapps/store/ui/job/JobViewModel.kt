// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.job

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.jobs.Resolver
import com.nbtapps.store.data.jobs.Unblocked
import com.nbtapps.store.data.jobs.unblockedBy
import com.nbtapps.store.data.packages.DownloadState
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.RemoveTarget
import com.nbtapps.store.ui.navigation.JobRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** Where the screen stands before a job has been started. */
sealed interface Review {
    /** Nothing was asked for: the screen shows the job there is. */
    data object None : Review

    data object Planning : Review

    /** Archives the plan needs are not on the phone yet. */
    data class NeedsDownload(val archives: List<Resolver.Needed>, val downloading: Boolean, val error: String?) : Review

    /** The agent's plan, for the user to read before anything changes. */
    data class Ready(val plan: Plan) : Review

    data class Failed(val reason: String) : Review
}

data class JobUiState(
    val review: Review = Review.None,
    /** The job to show: the one started from this screen, or with no request the one there is. */
    val job: JobView? = null,
    /** Whether there is a link to the head unit now. */
    val connected: Boolean = false,
    /** Display names by package id, for packages the catalogue or the head unit knows. */
    val names: Map<String, String> = emptyMap(),
    /** The request is a removal. */
    val removal: Boolean = false,
    /** Also delete the package's data folder. */
    val deleteData: Boolean = false,
    val starting: Boolean = false,
    /** Why the last start, stop, reconnect or restart failed. */
    val actionError: String? = null,
    /**
     * For a job that updated the head unit's agent and went well: what the new agent runs and the one before did
     * not, as the catalogue has it and the head unit now stands. Empty for any other job, and with no link.
     */
    val nowAvailable: List<Unblocked> = emptyList(),
) {
    fun name(id: String): String = names[id] ?: id
}

/**
 * One install, upgrade or removal, from the agent's plan to its result. The route says what is wanted; with
 * nothing wanted the screen follows the job that is running or ran last.
 */
@HiltViewModel
class JobViewModel @Inject constructor(
    savedStateHandle: SavedStateHandle,
    private val jobs: JobRepository,
    private val catalogue: CatalogueRepository,
    private val store: PackageStore,
    private val downloader: PackageDownloader,
) : ViewModel() {

    private val route = savedStateHandle.toRoute<JobRoute>()

    private data class Local(
        val review: Review,
        /** The plan a job was started from here. */
        val startedPlanId: String? = null,
        val deleteData: Boolean = false,
        val starting: Boolean = false,
        val actionError: String? = null,
    )

    private val hasRequest = route.installId != null || route.removeId != null
    private val local = MutableStateFlow(Local(if (hasRequest) Review.Planning else Review.None))
    private var planning: Job? = null

    val uiState: StateFlow<JobUiState> =
        combine(local, jobs.job, jobs.connection, catalogue.catalogue) { local, job, connection, catalogue ->
            val connected = connection as? ConnectionState.Connected
            val names = HashMap<String, String>()
            catalogue.index?.packages?.forEach { names[it.id] = it.name }
            connected?.installed?.forEach { names[it.id] = it.name }
            val shown = if (hasRequest) job?.takeIf { it.planId == local.startedPlanId } else job
            val result = shown?.job?.result
            val index = catalogue.index
            JobUiState(
                review = local.review,
                job = shown,
                nowAvailable = if (result != null && index != null && connected != null && connected.headUnitId == shown.headUnitId) {
                    unblockedBy(index, connected.installed, result)
                } else {
                    emptyList()
                },
                connected = connected != null,
                names = names,
                removal = route.removeId != null,
                deleteData = local.deleteData,
                starting = local.starting,
                actionError = local.actionError,
            )
        }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000), JobUiState(review = local.value.review))

    init {
        if (hasRequest) plan()
        // Archives that finish downloading while the screen waits for them lead straight on to the plan.
        viewModelScope.launch {
            combine(store.packages, downloader.downloads) { _, downloads -> downloads }.collect { downloads ->
                val waiting = local.value.review as? Review.NeedsDownload ?: return@collect
                val missing = waiting.archives.filter { needed -> store.packages.value.none { it.sha256 == needed.version.sha256 } }
                val states = missing.mapNotNull { downloads[it.version.sha256] }
                when {
                    missing.isEmpty() -> plan()
                    else -> local.update {
                        it.copy(
                            review = Review.NeedsDownload(
                                missing,
                                downloading = states.any { state -> state is DownloadState.Running },
                                error = states.filterIsInstance<DownloadState.Failed>().firstOrNull()?.reason,
                            ),
                        )
                    }
                }
            }
        }
    }

    /** Whether removing the package also deletes its data folder. The agent plans again, since that is part of the plan. */
    fun setDeleteData(delete: Boolean) {
        if (local.value.startedPlanId != null) return
        local.update { it.copy(deleteData = delete) }
        plan()
    }

    fun downloadMissing() {
        (local.value.review as? Review.NeedsDownload)?.archives?.forEach { downloader.download(it.pkg.id, it.version) }
    }

    /** Asks the agent for the plan again: after a refusal, or when the head unit has changed under the plan. */
    fun plan() {
        planning?.cancel()
        planning = viewModelScope.launch {
            local.update { it.copy(review = Review.Planning, actionError = null) }
            val review = prepare()
            local.update { it.copy(review = review) }
        }
    }

    fun start() {
        val plan = (local.value.review as? Review.Ready)?.plan ?: return
        if (local.value.starting) return
        viewModelScope.launch {
            local.update { it.copy(starting = true, actionError = null) }
            val result = jobs.start(plan)
            local.update {
                it.copy(
                    starting = false,
                    startedPlanId = if (result.isSuccess) plan.planId else null,
                    actionError = result.exceptionOrNull()?.message,
                )
            }
        }
    }

    fun stop() = act { jobs.cancel() }

    /** Opens the link again after it dropped. A job that paused goes on by itself once the link is back. */
    fun reconnect() {
        val headUnitId = uiState.value.job?.headUnitId ?: return
        act { jobs.connect(headUnitId) }
    }

    /** [kind] is `hmi` or `reboot`. */
    fun restart(kind: String) = act { jobs.restart(kind) }

    private fun act(action: suspend () -> Result<Unit>) {
        viewModelScope.launch {
            local.update { it.copy(actionError = null) }
            action().onFailure { failure -> local.update { it.copy(actionError = failure.message) } }
        }
    }

    private suspend fun prepare(): Review {
        val connected = jobs.connection.value as? ConnectionState.Connected
            ?: return Review.Failed("Connect to the head unit's agent first.")

        route.removeId?.let { id ->
            return jobs.plan(emptyList(), listOf(RemoveTarget(id, local.value.deleteData)), emptyList()).toReview()
        }

        val id = requireNotNull(route.installId)
        if (catalogue.catalogue.value.index == null) catalogue.refresh()
        val index = catalogue.catalogue.value.index ?: return Review.Failed("The catalogue couldn't be loaded.")
        val pkg = index.packages.find { it.id == id }
        val version = pkg?.versions?.find { it.version == route.installVersion }
            ?: return Review.Failed("The catalogue no longer has this version.")

        // The phone's own resolution, to know what to offer. The agent resolves again and has the last word.
        val needed = Resolver.resolve(index, Resolver.Needed(pkg, version), connected.installed, connected.info.agentVersion).needed
        // The store reads its folder when the app starts; after a cold start that may still be under way.
        store.awaitLoaded()
        val offers = needed.mapNotNull { item -> store.packages.value.find { it.sha256 == item.version.sha256 } }
        val missing = needed.filter { item -> offers.none { it.sha256 == item.version.sha256 } }
        if (missing.isNotEmpty()) return Review.NeedsDownload(missing, downloading = false, error = null)

        return jobs.plan(listOf(InstallTarget(id, version.version)), emptyList(), offers).toReview()
    }

    private fun Result<Plan>.toReview(): Review = fold(
        onSuccess = { Review.Ready(it) },
        onFailure = { Review.Failed(it.message ?: "The head unit didn't answer.") },
    )
}

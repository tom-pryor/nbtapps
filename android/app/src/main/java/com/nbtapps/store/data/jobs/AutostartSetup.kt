// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.data.PrereleaseRepository
import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.packages.DownloadState
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.AutostartReason
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.PackageNeed
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.VersionRange
import java.io.IOException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/** What having the agent start with the car takes, by what the agent says of it: see [autostartPlan]. */
sealed interface AutostartPlan {
    /** It is switched on, and something starts the agent. */
    data object AlreadyOn : AutostartPlan

    /** Something would start the agent: it only has to be switched on. */
    data object SwitchOn : AutostartPlan

    /** The iDrive plugin is wanting, or too old: [needs] is installed first, and then it is switched on. */
    data class InstallFirst(val needs: PackageNeed) : AutostartPlan

    /** Nothing the app can do would have the agent start with the car. [reason] is for the user. */
    data class CannotBe(val reason: String) : AutostartPlan
}

/**
 * What it takes to have the agent start with the car, where the agent says [autostart] of it: null for an agent
 * that has no such switch. Where the iDrive plugin is what is wanting, it is installed first; a plugin that is
 * new enough is never updated for this, even where the catalogue has a newer one.
 */
fun autostartPlan(autostart: Autostart?): AutostartPlan {
    if (autostart == null) return AutostartPlan.CannotBe(AGENT_WITHOUT_SWITCH)
    if (autostart.available) return if (autostart.on) AutostartPlan.AlreadyOn else AutostartPlan.SwitchOn
    val needs = autostart.needs
    return when {
        needs != null && (autostart.reason == AutostartReason.PLUGIN_MISSING || autostart.reason == AutostartReason.PLUGIN_TOO_OLD) ->
            AutostartPlan.InstallFirst(needs)
        else -> AutostartPlan.CannotBe(AgentText.autostartReason(autostart.reason))
    }
}

/** Why an agent from before the switch cannot be set up to start with the car. */
internal const val AGENT_WITHOUT_SWITCH = "The head unit's agent is too old to be switched to start with the car. Update the agent first."

/** Which version of the iDrive plugin to install for [AutostartPlan.InstallFirst]: see [pluginToInstall]. */
sealed interface PluginChoice {
    data class Install(val pkg: CataloguePackage, val version: PackageVersion) : PluginChoice

    /** The catalogue has none that would do. [reason] is for the user. */
    data class Unavailable(val reason: String) : PluginChoice
}

/**
 * The version of the package [needs] names to install, from [index], on a head unit that has [installed] and runs
 * agent [agentVersion]: the one the package's page offers there ([resolvedStatus]), by the package's own switch
 * among [switchedOn]. It has to run on that agent, lie inside the range [needs] gives, and wait for no pre-release
 * that is switched off, the package's own or another's; otherwise there is none, and the reason says why.
 */
fun pluginToInstall(
    index: CatalogueIndex,
    needs: PackageNeed,
    installed: List<InstalledPackage>,
    agentVersion: String?,
    switchedOn: Set<String>,
): PluginChoice {
    val pkg = index.packages.find { it.id == needs.id } ?: return PluginChoice.Unavailable(NOT_IN_CATALOGUE)
    val status = resolvedStatus(index, pkg, installed, agentVersion, switchedOn)?.status
        ?: return PluginChoice.Unavailable(NOT_IN_CATALOGUE)
    return when {
        status.needsSwitch -> PluginChoice.Unavailable(
            "The iDrive plugin is out only in pre-release versions. To install it, turn on its pre-release versions first.",
        )
        !status.suits -> PluginChoice.Unavailable("Every iDrive plugin in the catalogue needs a newer agent than the head unit has. Update the agent first.")
        !VersionRange.matches(status.offered.version, needs.version) ->
            PluginChoice.Unavailable("The catalogue has no iDrive plugin that would start this agent.")
        status.asks.isNotEmpty() -> PluginChoice.Unavailable(waitingFor(status.asks))
        else -> PluginChoice.Install(pkg, status.offered)
    }
}

private const val NOT_IN_CATALOGUE = "The catalogue doesn't have the iDrive plugin."

/** Where setting up one head unit to start the agent with the car stands: see [AutostartSetup]. */
sealed interface AutostartStep {
    /** What the agent says of it is being looked at. */
    data object Checking : AutostartStep

    /** The iDrive plugin is being downloaded to the phone. */
    data object Downloading : AutostartStep

    /** The iDrive plugin is being installed: [planId] is its job's, once the job has started. */
    data class Installing(val planId: String? = null) : AutostartStep

    /** The agent is being switched to start with the car. */
    data object SwitchingOn : AutostartStep

    /**
     * The agent starts with the car. [changed]: the set-up switched it on, and may have installed the plugin; false
     * where it did so already. [restartError]: why the last ask to restart iDrive was refused.
     */
    data class Done(val changed: Boolean, val restartError: String? = null) : AutostartStep

    /** iDrive was asked to restart, and took it. The app does not connect again by itself. */
    data object Restarting : AutostartStep

    /** The set-up stopped here. [reason] is for the user; [AutostartSetup.retry] runs it again from the top. */
    data class Failed(val reason: String) : AutostartStep
}

/** The set-up of [headUnitId], this app's id of the head unit, and where it stands. */
data class AutostartSetupState(val headUnitId: String, val step: AutostartStep) {
    /** Nothing more happens by itself: it is done, iDrive restarts, or it failed. */
    val ended: Boolean get() = step is AutostartStep.Done || step is AutostartStep.Restarting || step is AutostartStep.Failed

    /** It is under way. */
    val running: Boolean get() = !ended
}

/**
 * Sets up a head unit to start the agent with the car, as the pairing screens offer once the phone is paired: it
 * installs the iDrive plugin where the agent says it is wanting ([autostartPlan]), without showing the plan, since
 * the user's tick is the consent, and then switches it on. The order is the agent's: it refuses to be switched on
 * until the plugin is there, and it never restarts iDrive itself, which is the user's to ask for ([restart]).
 *
 * It runs on the app's scope, so it goes on when the screen that began it is left, and it is kept in memory only:
 * a process that is stopped forgets it. Where it stands is [state], for one head unit at a time.
 */
class AutostartSetup(
    private val jobs: JobRepository,
    private val catalogue: CatalogueRepository,
    private val store: PackageStore,
    private val downloader: PackageDownloader,
    private val prereleases: PrereleaseRepository,
    private val scope: CoroutineScope,
    /**
     * How long, after the plugin's job has ended, the app waits for the agent to say that the switch can be had,
     * before it asks the agent again itself. The repository asks after every job that ends; this is in case that
     * answer is lost. In milliseconds; a test shortens it.
     */
    private val settleMs: Long = 10_000,
) {
    private val current = MutableStateFlow<AutostartSetupState?>(null)

    /** The set-up that runs, or the last one, until it is dismissed. Null when there is none. */
    val state: StateFlow<AutostartSetupState?> = current.asStateFlow()

    private val lock = Any()

    /** The run or the ask to restart that is under way, if any. */
    private var work: Job? = null

    /** Rises with each run, so one that was replaced publishes nothing more. */
    private var generation = 0

    /**
     * Sets up the head unit [headUnitId] from the top. A set-up that is under way for it already is left to go on;
     * one for another head unit is given up for this one. The app's link has to be to [headUnitId].
     */
    fun begin(headUnitId: String) {
        synchronized(lock) {
            if (current.value?.headUnitId == headUnitId && work?.isActive == true) return
            startRun(headUnitId)
        }
    }

    /**
     * Runs the set-up there was again, from the top: where the plugin went in the last time, it goes straight on to
     * the switch. The agent is asked first how things stand, so that what was put right on the head unit since is
     * seen. Nothing happens while one is under way.
     */
    fun retry() {
        synchronized(lock) {
            val last = current.value ?: return
            if (work?.isActive == true) return
            startRun(last.headUnitId, fresh = true)
        }
    }

    /**
     * Asks the agent to restart iDrive, once the set-up is done: the plugin is loaded only then. That restarts the
     * whole head unit, and the link with it; the app does not connect again by itself. A refusal is shown in
     * [AutostartStep.Done.restartError].
     */
    fun restart() {
        synchronized(lock) {
            val done = current.value?.step as? AutostartStep.Done ?: return
            if (work?.isActive == true) return
            val mine = ++generation
            publish(mine, done.copy(restartError = null))
            work = scope.launch {
                val failure = jobs.restart("hmi").exceptionOrNull()
                publish(mine, if (failure == null) AutostartStep.Restarting else done.copy(restartError = failure.message ?: "The head unit didn't answer"))
            }
        }
    }

    /**
     * Forgets a set-up that has ended: with [headUnitId], only that head unit's. One that is under way goes on. It
     * is what the user's Hide does, and what follows where the set-up no longer says how things stand: the phone
     * was paired again without it, or the switch was thrown by hand.
     */
    fun dismiss(headUnitId: String? = null) {
        synchronized(lock) {
            val last = current.value ?: return
            if (headUnitId != null && last.headUnitId != headUnitId) return
            if (last.ended && work?.isActive != true) current.value = null
        }
    }

    /**
     * Begins a run for [headUnitId] in place of whatever there was. [fresh]: the agent is asked how things stand
     * before anything else. Call with [lock] held.
     */
    private fun startRun(headUnitId: String, fresh: Boolean = false) {
        work?.cancel()
        val mine = ++generation
        current.value = AutostartSetupState(headUnitId, AutostartStep.Checking)
        work = scope.launch {
            val end = run(headUnitId, fresh) { step -> publish(mine, step) }
            publish(mine, end)
        }
    }

    /** Shows [step] as where the set-up stands, unless the run [mine] has been replaced meanwhile. */
    private fun publish(mine: Int, step: AutostartStep) {
        synchronized(lock) {
            val last = current.value ?: return
            if (mine == generation) current.value = last.copy(step = step)
        }
    }

    /** The link to [headUnitId], if the app has one up. */
    private fun connectedTo(headUnitId: String): ConnectionState.Connected? =
        (jobs.connection.value as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }

    /** Why [failure] stopped the set-up of [headUnitId]: a link that is gone is said as such, whatever failed on it. */
    private fun failed(headUnitId: String, failure: Throwable?, otherwise: String): AutostartStep.Failed =
        AutostartStep.Failed(if (connectedTo(headUnitId) == null) LINK_LOST else failure?.message ?: otherwise)

    /**
     * The set-up of [headUnitId] from the top, telling [show] each step as it is reached; returns how it ended.
     * With [fresh] it goes by what the agent says now, where otherwise it goes by what the link last read.
     */
    private suspend fun run(headUnitId: String, fresh: Boolean, show: (AutostartStep) -> Unit): AutostartStep {
        // A refresh that fails leaves what was read before, or no link, which the next line sees.
        if (fresh && connectedTo(headUnitId) != null) jobs.refresh()
        val connected = connectedTo(headUnitId) ?: return AutostartStep.Failed(NO_LINK)
        return when (val plan = autostartPlan(connected.autostart)) {
            AutostartPlan.AlreadyOn -> AutostartStep.Done(changed = false)
            AutostartPlan.SwitchOn -> switchOn(headUnitId, show)
            is AutostartPlan.CannotBe -> AutostartStep.Failed(plan.reason)
            is AutostartPlan.InstallFirst -> install(headUnitId, connected, plan.needs, show) ?: settle(headUnitId) ?: switchOn(headUnitId, show)
        }
    }

    private suspend fun switchOn(headUnitId: String, show: (AutostartStep) -> Unit): AutostartStep {
        show(AutostartStep.SwitchingOn)
        if (connectedTo(headUnitId) == null) return AutostartStep.Failed(LINK_LOST)
        val failure = jobs.setAutostart(true).exceptionOrNull() ?: return AutostartStep.Done(changed = true)
        return failed(headUnitId, failure, "The head unit didn't answer")
    }

    /**
     * Installs the iDrive plugin that [needs] names, as the job screen would, without the plan being shown: the
     * version [pluginToInstall] chooses, with what it depends on, downloaded first where the phone lacks it.
     * Returns null once the job has succeeded, or how the set-up failed.
     */
    private suspend fun install(
        headUnitId: String,
        connected: ConnectionState.Connected,
        needs: PackageNeed,
        show: (AutostartStep) -> Unit,
    ): AutostartStep.Failed? {
        if (catalogue.catalogue.value.index == null) catalogue.refresh()
        val index = catalogue.catalogue.value.index ?: return AutostartStep.Failed("The catalogue couldn't be loaded. Check the phone's internet connection, then retry.")
        val switchedOn = prereleases.switchedOn.first()
        val installed = connected.installed
        val agentVersion = connected.info.agentVersion
        val choice = when (val chosen = pluginToInstall(index, needs, installed, agentVersion, switchedOn)) {
            is PluginChoice.Unavailable -> return AutostartStep.Failed(chosen.reason)
            is PluginChoice.Install -> chosen
        }
        val prep = installPrep(index, choice.pkg, choice.version, installed, agentVersion, switchedOn)
        prep.waiting?.let { return AutostartStep.Failed(it) }

        // The store reads its folder when the app starts; after a cold start that may still be under way.
        store.awaitLoaded()
        val missing = prep.needed.filter { item -> store.packages.value.none { it.sha256 == item.version.sha256 } }
        if (missing.isNotEmpty()) {
            show(AutostartStep.Downloading)
            download(missing)?.let { return it }
        }
        val offers = prep.needed.map { item ->
            store.packages.value.find { it.sha256 == item.version.sha256 } ?: return AutostartStep.Failed("The iDrive plugin isn't on the phone. Retry to download it again.")
        }

        show(AutostartStep.Installing())
        if (connectedTo(headUnitId) == null) return AutostartStep.Failed(LINK_LOST)
        val plan = jobs.plan(listOf(InstallTarget(choice.pkg.id, choice.version.version)), emptyList(), offers)
            .getOrElse { return failed(headUnitId, it, "The head unit didn't answer") }
        if (!plan.runnable) return AutostartStep.Failed(notRunnable(plan, index))
        val planId = requireNotNull(plan.planId)
        jobs.start(plan).exceptionOrNull()?.let { return failed(headUnitId, it, "The install couldn't be started") }
        show(AutostartStep.Installing(planId))

        // The job's end, or the link's, whichever comes first.
        val view = combine(jobs.job, jobs.connection) { job, _ -> job }
            .first { job -> connectedTo(headUnitId) == null || (job != null && job.planId == planId && (job.ended || !job.needsPhone)) }
        if (view == null || view.planId != planId || !view.ended) return AutostartStep.Failed(LINK_LOST)
        if (view.job.state == JobState.SUCCEEDED) return null
        return AutostartStep.Failed(jobFailure(view, index))
    }

    /** Downloads [missing] to the phone, and waits for all of it. Returns null once it is there, or how it failed. */
    private suspend fun download(missing: List<Resolver.Needed>): AutostartStep.Failed? {
        missing.forEach { downloader.download(it.pkg.id, it.version) }
        val outcome = combine(store.packages, downloader.downloads) { packages, downloads ->
            val left = missing.filter { item -> packages.none { it.sha256 == item.version.sha256 } }
            val failure = left.firstNotNullOfOrNull { downloads[it.version.sha256] as? DownloadState.Failed }
            when {
                left.isEmpty() -> Result.success(Unit)
                failure != null -> Result.failure(IOException(failure.reason))
                // Still downloading.
                else -> null
            }
        }.filterNotNull().first()
        val failure = outcome.exceptionOrNull() ?: return null
        return AutostartStep.Failed("The iDrive plugin couldn't be downloaded: ${failure.message}")
    }

    /**
     * Waits, after the plugin's job, for the agent to say that the switch can be had, as the repository asks after
     * every job; and asks it once more itself if that does not come. Returns null once it can, or how it failed.
     */
    private suspend fun settle(headUnitId: String): AutostartStep.Failed? {
        val ready = { connectedTo(headUnitId)?.autostart?.available == true }
        withTimeoutOrNull(settleMs) { jobs.connection.first { ready() || connectedTo(headUnitId) == null } }
        if (!ready() && connectedTo(headUnitId) != null) jobs.refresh()
        if (ready()) return null
        val connected = connectedTo(headUnitId) ?: return AutostartStep.Failed(LINK_LOST)
        return AutostartStep.Failed(connected.autostart?.let { AgentText.autostartReason(it.reason) } ?: AGENT_WITHOUT_SWITCH)
    }

    private fun nameOf(index: CatalogueIndex, id: String): String = index.packages.find { it.id == id }?.name ?: id

    /** Why the agent's [plan] for the plugin cannot run, in the app's words. */
    private fun notRunnable(plan: Plan, index: CatalogueIndex): String {
        plan.problems.firstOrNull()?.let { return AgentText.problem(it.code, nameOf(index, it.id)) }
        plan.missing.firstOrNull()?.let { return "${nameOf(index, it.requiredBy)} needs ${nameOf(index, it.id)}, and the catalogue has no version of it for this head unit" }
        return "The head unit can't install the iDrive plugin"
    }

    /** Why the plugin's job, which ended as [view] shows, did not succeed, in the app's words. */
    private fun jobFailure(view: JobView, index: CatalogueIndex): String {
        val step = view.job.result?.steps?.firstOrNull { it.error != null }
        step?.error?.let { return AgentText.problem(it.code, nameOf(index, step.id)) }
        return when (view.job.state) {
            JobState.CANCELLED -> "The install of the iDrive plugin was stopped"
            else -> "The install of the iDrive plugin failed"
        }
    }

    private companion object {
        const val NO_LINK = "The app has no link to the head unit. Connect to it, then retry."
        const val LINK_LOST = "The app lost its link to the head unit. Connect again, then retry."
    }
}

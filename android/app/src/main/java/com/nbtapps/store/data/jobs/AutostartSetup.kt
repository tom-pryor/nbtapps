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
import com.nbtapps.store.model.CheckProblem
import com.nbtapps.store.model.CheckState
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.PackageNeed
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.VersionRange
import java.io.IOException
import kotlinx.coroutines.CompletableDeferred
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
    /** What the agent says of it is being looked at, and what its checks found. */
    data object Checking : AutostartStep

    /**
     * The agent starts with the car, and last the checks found [problem] on the head unit, whose fix changes files:
     * the set-up waits for the fix's own button ([AutostartSetup.confirmRemoval], once what it changes has
     * been shown and confirmed) or Skip ([AutostartSetup.skipRemoval]). [changed]: the files changed since the list
     * the user was shown last. [switchedOn]: the set-up switched the agent on, as in [Done.changed].
     */
    data class AskRemoval(val problem: CheckProblem, val changed: Boolean = false, val switchedOn: Boolean = false) : AutostartStep

    /** The user confirmed what the fix of [problem] changes, and it runs. [switchedOn]: as in [AskRemoval]. */
    data class Removing(val problem: CheckProblem, val switchedOn: Boolean = false) : AutostartStep

    /** The iDrive plugin is being downloaded to the phone. */
    data object Downloading : AutostartStep

    /** The iDrive plugin is being installed: [planId] is its job's, once the job has started. */
    data class Installing(val planId: String? = null) : AutostartStep

    /** The agent is being switched to start with the car. */
    data object SwitchingOn : AutostartStep

    /**
     * The agent starts with the car, and the questions about what was installed by hand are answered. [changed]: the
     * set-up switched it on, and may have installed the plugin; false where it did so already. [moved] and
     * [removed]: how many folders the fixes the user confirmed moved aside, and how many paths they removed. [left]:
     * a fix left something, which the head unit's page shows. [fixFailed]: why a fix could not be done, which the
     * head unit's page still has. [restartError]: why the last ask to restart iDrive was refused.
     */
    data class Done(
        val changed: Boolean,
        val moved: Int = 0,
        val removed: Int = 0,
        val left: Boolean = false,
        val fixFailed: String? = null,
        val restartError: String? = null,
    ) : AutostartStep

    /** iDrive was asked to restart, and took it. The app does not connect again by itself. */
    data object Restarting : AutostartStep

    /** The set-up stopped here. [reason] is for the user; [AutostartSetup.retry] runs it again from the top. */
    data class Failed(val reason: String) : AutostartStep
}

/**
 * The stages a set-up goes through, in the order it goes through them, as [AutostartSetupState.stages] records them.
 * A run need not go through all of them: see there.
 */
enum class AutostartStage {
    /** What the agent says of it is looked at, and its checks are run: every run begins with this. */
    CHECKING,

    /** The iDrive plugin is chosen from the catalogue, and downloaded to the phone where the phone lacks it. */
    GETTING_PLUGIN,

    /** The iDrive plugin is installed on the head unit. */
    INSTALLING_PLUGIN,

    /** The agent is switched to start with the car. */
    SWITCHING_ON,
}

/** The stage that [AutostartSetup] is in while it shows this step, or null for a step that is no stage's. */
private val AutostartStep.stage: AutostartStage?
    get() = when (this) {
        AutostartStep.Checking -> AutostartStage.CHECKING
        AutostartStep.Downloading -> AutostartStage.GETTING_PLUGIN
        is AutostartStep.Installing -> AutostartStage.INSTALLING_PLUGIN
        AutostartStep.SwitchingOn -> AutostartStage.SWITCHING_ON
        else -> null
    }

/**
 * The set-up of [headUnitId], this app's id of the head unit, and where it stands.
 *
 * [stages] are the stages the run that shows [step] has begun, in order; the last is the one it is in, or was in
 * when it ended or began to ask. A stage that is not there was not needed, or not reached: where the head unit has
 * the iDrive plugin it is neither got nor installed, and where the agent starts with the car already it is not
 * switched on. A run begun again, by [AutostartSetup.retry] too, begins the list again.
 */
data class AutostartSetupState(
    val headUnitId: String,
    val step: AutostartStep,
    val stages: List<AutostartStage> = emptyList(),
) {
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
 * The agent's checks are run first ([JobRepository.ensureChecked]), since the agent does not answer them while a
 * step installs. Once the agent starts with the car, what they found whose fix changes files is put to the user,
 * last, one check at a time, in the agent's order ([AutostartStep.AskRemoval]): the tick is no consent to change
 * anything there. The set-up waits for the answer as long as it takes. The fix runs once the user has seen what it
 * moves aside and removes and said yes to it; Skip leaves it all as it is. Neither undoes what the set-up did, and
 * nor does a fix that fails or leaves something: the agent starts with the car by then. One restart at the end
 * covers the plugin and the fix. A set-up that stops before the questions asks none; the head unit's page has them.
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
    /**
     * How long the plugin's choice waits for a newer index where an old one is held, which it then goes by. The fetch
     * itself goes on. In milliseconds; a test sets it.
     */
    private val freshenMs: Long = 5_000,
) {
    private val current = MutableStateFlow<AutostartSetupState?>(null)

    /** The set-up that runs, or the last one, until it is dismissed. Null when there is none. */
    val state: StateFlow<AutostartSetupState?> = current.asStateFlow()

    private val lock = Any()

    /**
     * The run or the ask to restart that is under way, if any. It is let go as it shows how it ended ([finish]), a
     * moment before its coroutine returns, so that a Retry, a begin or a Hide that comes on seeing that end is taken,
     * not lost to that moment. For a run that is when the set-up has [ended][AutostartSetupState.ended]; the state
     * alone would not do for an ask to restart, which shows the set-up done throughout.
     */
    private var work: Job? = null

    /** Whether a run or an ask to restart is under way: see [work]. Call with [lock] held. */
    private val underWay: Boolean get() = work?.isActive == true

    /** Rises with each run, so one that was replaced publishes nothing more. */
    private var generation = 0

    /** What the user answers to [AutostartStep.AskRemoval]. */
    private sealed interface Answer {
        /** Remove, with the token of the list the user said yes to; null where the check finds nothing now. */
        data class Remove(val token: String?) : Answer

        data object Skip : Answer
    }

    /** The question that is open: of the run [generation], about the check [id]. */
    private class Question(val generation: Int, val id: String) {
        val answer = CompletableDeferred<Answer>()
    }

    private var question: Question? = null

    /**
     * Sets up the head unit [headUnitId] from the top. A set-up that is under way for it already is left to go on;
     * one for another head unit is given up for this one. The app's link has to be to [headUnitId].
     */
    fun begin(headUnitId: String) {
        synchronized(lock) {
            if (current.value?.headUnitId == headUnitId && underWay) return
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
            if (underWay) return
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
            if (underWay) return
            val mine = ++generation
            publish(mine, done.copy(restartError = null))
            work = scope.launch {
                val failure = jobs.restart("hmi").exceptionOrNull()
                finish(mine, if (failure == null) AutostartStep.Restarting else done.copy(restartError = failure.message ?: "The head unit didn't answer"))
            }
        }
    }

    /**
     * Answers Remove to the question about the check [id], once the user has seen its list and said yes to it:
     * [token] is that list's, or null where the check finds nothing now. Does nothing unless the set-up that runs
     * asks just that.
     */
    fun confirmRemoval(id: String, token: String?) = answer(id, Answer.Remove(token))

    /** Answers Skip to the question that is open, if there is one: the files stay. */
    fun skipRemoval() {
        synchronized(lock) { question?.id }?.let { answer(it, Answer.Skip) }
    }

    private fun answer(id: String, answer: Answer) {
        synchronized(lock) {
            val open = question ?: return
            if (open.generation != generation || open.id != id) return
            question = null
            open.answer.complete(answer)
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
            if (last.ended && !underWay) current.value = null
        }
    }

    /**
     * Begins a run for [headUnitId] in place of whatever there was. [fresh]: the agent is asked how things stand
     * before anything else. Call with [lock] held.
     */
    private fun startRun(headUnitId: String, fresh: Boolean = false) {
        work?.cancel()
        question = null
        val mine = ++generation
        current.value = AutostartSetupState(headUnitId, AutostartStep.Checking, listOf(AutostartStage.CHECKING))
        work = scope.launch {
            val end = run(headUnitId, fresh, mine) { step -> publish(mine, step) }
            finish(mine, end)
        }
    }

    /**
     * Shows [step] as how the run or the ask to restart [mine] ended, and lets it go ([work]), unless it has been
     * replaced meanwhile.
     */
    private fun finish(mine: Int, step: AutostartStep) {
        synchronized(lock) {
            publish(mine, step)
            if (mine == generation) work = null
        }
    }

    /**
     * Shows [step] as where the set-up stands, unless the run [mine] has been replaced meanwhile. A step of a stage
     * not yet begun begins it ([AutostartSetupState.stages]).
     */
    private fun publish(mine: Int, step: AutostartStep) {
        synchronized(lock) {
            val last = current.value ?: return
            if (mine == generation) current.value = last.copy(step = step, stages = last.stages.with(step.stage))
        }
    }

    /** Records that the run [mine] has begun [stage], where no step of its own says so, unless the run has been replaced. */
    private fun enter(mine: Int, stage: AutostartStage) {
        synchronized(lock) {
            val last = current.value ?: return
            if (mine == generation) current.value = last.copy(stages = last.stages.with(stage))
        }
    }

    /** These stages with [stage] added at the end, if it is not among them already. */
    private fun List<AutostartStage>.with(stage: AutostartStage?): List<AutostartStage> =
        if (stage == null || stage in this) this else this + stage

    /** The link to [headUnitId], if the app has one up. */
    private fun connectedTo(headUnitId: String): ConnectionState.Connected? =
        (jobs.connection.value as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }

    /** Why [failure] stopped the set-up of [headUnitId]: a link that is gone is said as such, whatever failed on it. */
    private fun failed(headUnitId: String, failure: Throwable?, otherwise: String): AutostartStep.Failed =
        AutostartStep.Failed(if (connectedTo(headUnitId) == null) LINK_LOST else failure?.message ?: otherwise)

    /**
     * The set-up of [headUnitId] from the top, as the run [mine], telling [show] each step as it is reached; returns
     * how it ended. With [fresh] it goes by what the agent says now, where otherwise it goes by what the link last read.
     */
    private suspend fun run(headUnitId: String, fresh: Boolean, mine: Int, show: (AutostartStep) -> Unit): AutostartStep {
        // Up to the questions every step needs the link, the plugin's download too, since its install follows: the
        // app leaving the foreground meanwhile does not end it. A question waits for the user, and so does not.
        // What the questions are about is read within the same use: with the app away the link goes as the use ends,
        // and without a link there is nothing to read.
        val (end, checks) = jobs.usingLink {
            val end = setUp(headUnitId, fresh, mine, show)
            // As they are kept now; asked for again, in case the first run was refused.
            end to if (end is AutostartStep.Done) jobs.ensureChecked() else null
        }
        if (end !is AutostartStep.Done) return end
        // Last, the questions.
        var done: AutostartStep.Done = end
        for (problem in checks?.open.orEmpty().filter { it.changesFiles }) {
            done = removal(headUnitId, problem, done, mine, show) ?: return done
            if (done.fixFailed != null) break
        }
        return done
    }

    /** What [run] does before the questions: everything up to the agent starting with the car, or how that failed. */
    private suspend fun setUp(headUnitId: String, fresh: Boolean, mine: Int, show: (AutostartStep) -> Unit): AutostartStep {
        // A refresh that fails leaves what was read before, or no link, which the next line sees.
        if (fresh && connectedTo(headUnitId) != null) jobs.refresh()
        val connected = connectedTo(headUnitId) ?: return AutostartStep.Failed(NO_LINK)
        // Before the install: the agent does not run the checks while a step installs.
        jobs.ensureChecked()
        return when (val plan = autostartPlan(connected.autostart)) {
            AutostartPlan.AlreadyOn -> AutostartStep.Done(changed = false)
            AutostartPlan.SwitchOn -> switchOn(headUnitId, show)
            is AutostartPlan.CannotBe -> AutostartStep.Failed(plan.reason)
            is AutostartPlan.InstallFirst -> {
                // Choosing the plugin is part of getting it, whether or not it has to be downloaded.
                enter(mine, AutostartStage.GETTING_PLUGIN)
                install(headUnitId, connected, plan.needs, show) ?: settle(headUnitId) ?: switchOn(headUnitId, show)
            }
        }
    }

    /**
     * Puts [problem] to the user, as the run [mine], with the set-up [done] as far as it went, and does what the
     * answer says. Returns [done] with what the fix did added, or with why it could not be done; null where the run
     * was replaced meanwhile. Nothing here fails the set-up: the agent starts with the car by now.
     */
    private suspend fun removal(
        headUnitId: String,
        problem: CheckProblem,
        done: AutostartStep.Done,
        mine: Int,
        show: (AutostartStep) -> Unit,
    ): AutostartStep.Done? {
        var asked = problem
        var changed = false
        while (true) {
            val answer = ask(mine, AutostartStep.AskRemoval(asked, changed, done.changed), show) ?: return null
            val token = when (answer) {
                Answer.Skip -> return done
                is Answer.Remove -> answer.token ?: return done
            }
            show(AutostartStep.Removing(asked, done.changed))
            if (connectedTo(headUnitId) == null) return done.copy(fixFailed = FIX_LINK_LOST)
            val result = jobs.fixCheck(asked.id, token)
            val failure = result.exceptionOrNull()
            if (failure is AgentRefusal && failure.code == "stale") {
                // The list changed since the user saw it: it is read again, and the user is asked about the new one.
                val again = jobs.runCheck(asked.id).getOrElse { return done.copy(fixFailed = fixFailure(headUnitId, it)) }
                asked = CheckProblem.of(again) ?: return done
                changed = true
                continue
            }
            if (failure != null) return done.copy(fixFailed = fixFailure(headUnitId, failure))
            val outcome = result.getOrThrow()
            return done.copy(
                moved = done.moved + outcome.moved.size,
                removed = done.removed + outcome.removed,
                left = done.left || outcome.state == CheckState.PROBLEM,
            )
        }
    }

    /** Why a fix could not be done, for [Done.fixFailed][AutostartStep.Done.fixFailed]. */
    private fun fixFailure(headUnitId: String, failure: Throwable): String =
        if (connectedTo(headUnitId) == null) FIX_LINK_LOST else failure.message ?: "The head unit didn't answer."

    /**
     * Shows [step] as the question of the run [mine] and waits for the answer. Null where the run was replaced
     * meanwhile, which then publishes nothing more.
     */
    private suspend fun ask(mine: Int, step: AutostartStep.AskRemoval, show: (AutostartStep) -> Unit): Answer? {
        val open = Question(mine, step.problem.id)
        synchronized(lock) {
            if (mine != generation) return null
            question = open
        }
        show(step)
        try {
            return open.answer.await()
        } finally {
            synchronized(lock) { if (question === open) question = null }
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
        if (catalogue.catalogue.value.index == null) {
            catalogue.refresh()
        } else {
            withTimeoutOrNull(freshenMs) { catalogue.refreshIfStale() }
        }
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

        /** Why a fix could not be done when the link was gone: the agent starts with the car by then. */
        const val FIX_LINK_LOST = "The app lost its link to the head unit before it could put this right."
    }
}

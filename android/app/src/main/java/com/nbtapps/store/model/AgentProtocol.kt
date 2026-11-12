// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.model

import kotlinx.serialization.Serializable

// The parameters and results of the phone and agent protocol, as the plan document's "Message reference" tab
// sets them. Fields the app does not know are ignored when these are read, and a field with no value is left
// out when they are written: null is never sent.

/** Both sides' `hello`: sent by the side that dialled, answered with the other side's own values. */
@Serializable
data class Hello(
    val protocolMin: Int,
    val protocolMax: Int,
    /** `agent`, or the kind of controller: `phone`, `plugin` or `cli`. */
    val role: String,
    val phoneId: String? = null,
    val name: String? = null,
    val headUnitId: String? = null,
    val agentVersion: String? = null,
    /** The largest payload the sender accepts, from 1,024 to 65,536. The link uses the lower value both ways. */
    val maxFrame: Int,
    val capabilities: List<String> = emptyList(),
)

/** Free bytes on the file systems the agent writes to. */
@Serializable
data class FreeSpace(
    val app: Long = 0,
    val data: Long = 0,
    val plugins: Long = 0,
    val staging: Long = 0,
)

/** The result of `info.get`. */
@Serializable
data class AgentInfo(
    val agentVersion: String,
    val headUnitId: String,
    val manifestVersion: Int,
    val free: FreeSpace = FreeSpace(),
    val restartNeeded: String = RestartKind.NONE,
)

/**
 * The result of `autostart.get` and of `autostart.set`: whether the agent is started with the head unit. Only an
 * agent that lists the capability `autostart` in its `hello` has the two methods.
 *
 * What does the starting is the iDrive plugin, which is a package of its own. Without it on the head unit, in a
 * version that starts the agent, there is nothing to switch on, and the agent says which package would do.
 */
@Serializable
data class Autostart(
    /** It is switched on: the file that asks for it is on the head unit. */
    val on: Boolean,
    /** Something would start the agent if it were switched on. Switching on is refused where this is false. */
    val available: Boolean,
    /** With [available] false, why: one of [AutostartReason]'s. */
    val reason: String? = null,
    /** Where it is the iDrive plugin that is wanting: its package, and the versions that would do. */
    val needs: PackageNeed? = null,
    /**
     * The starts with the head unit on record that no agent outlasted by five minutes. One is on record for the
     * first minutes after every start; after a few in a row the plugin starts nothing more. Switching on, also
     * where it is on already, forgets them.
     */
    val starts: Int = 0,
)

/** A package that has to be installed for something to work: [version] is the range that would do. */
@Serializable
data class PackageNeed(val id: String, val version: String = "")

/** Why starting with the head unit is not to be had: the `reason` of [Autostart], and the code `autostart.set` is refused with. */
object AutostartReason {
    /** The agent was not put onto the head unit with a starter, so nothing can start it. */
    const val NO_LAUNCHER = "no-launcher"

    /** The iDrive plugin is not installed as a package. */
    const val PLUGIN_MISSING = "plugin-missing"

    /** The iDrive plugin that is installed is from before the versions that start the agent. */
    const val PLUGIN_TOO_OLD = "plugin-too-old"
}

/** One package the head unit has installed, from `packages.list`. */
@Serializable
data class InstalledPackage(
    val id: String,
    val name: String,
    val version: String,
    /** The public key that signed the installed version, in hex. */
    val key: String = "",
    /** The risk flags, worked out by the agent from the manifest. */
    val flags: List<String> = emptyList(),
)

@Serializable
data class InstalledPage(
    val packages: List<InstalledPackage> = emptyList(),
    val more: Boolean = false,
)

/** A package key the agent trusts, from `sources.list`. */
@Serializable
data class TrustedSource(
    val name: String,
    val key: String,
    /** Built into the agent, so it cannot be removed. */
    val embedded: Boolean = false,
)

@Serializable
data class TrustedSources(val sources: List<TrustedSource> = emptyList())

/** An archive the phone can supply for a job. `id` and `version` are hints the agent checks against the manifest. */
@Serializable
data class Offer(
    val id: String,
    val version: String,
    val sha256: String,
    val size: Long,
    val manifestSha256: String,
    /** The contents of the archive's `manifest.sig`, in hex. */
    val signature: String,
)

@Serializable
data class InstallTarget(val id: String, val version: String)

@Serializable
data class RemoveTarget(val id: String, val deleteData: Boolean = false)

/** The parameters of `job.plan`. */
@Serializable
data class PlanRequest(
    val install: List<InstallTarget> = emptyList(),
    val remove: List<RemoveTarget> = emptyList(),
    val offers: List<Offer> = emptyList(),
)

/** One step of a plan: `remove`, `install` or `upgrade`. */
@Serializable
data class PlanStep(
    val action: String,
    val id: String,
    val version: String = "",
    val oldVersion: String? = null,
    val sha256: String? = null,
)

/** A dependency with no usable offer. [version] is the range asked for, or null for any version. */
@Serializable
data class MissingDependency(
    val id: String,
    val version: String? = null,
    val requiredBy: String = "",
)

/** Why a plan cannot run. [code] is from the protocol's list of plan problems. */
@Serializable
data class PlanProblem(
    val code: String,
    val id: String = "",
    val message: String = "",
)

/** The result of `job.plan`. Nothing on the head unit has changed. */
@Serializable
data class Plan(
    /** 16 hex characters. Present only when [missing] and [problems] are both empty. */
    val planId: String? = null,
    val steps: List<PlanStep> = emptyList(),
    val missing: List<MissingDependency> = emptyList(),
    val problems: List<PlanProblem> = emptyList(),
    val flags: List<String> = emptyList(),
    /** Archive bytes the agent still has to fetch, after what its blob cache holds. */
    val fetchBytes: Long = 0,
    /** Uncompressed size of the files the job places. */
    val installBytes: Long = 0,
    val restart: String = RestartKind.NONE,
) {
    val runnable: Boolean get() = planId != null && missing.isEmpty() && problems.isEmpty()
    val riskFlags: List<RiskFlag> get() = flags.mapNotNull(RiskFlag::fromId)
}

/** The restart a package, a plan or a finished job asks for. */
object RestartKind {
    const val NONE = "none"
    const val HMI = "hmi-restart"
    const val REBOOT = "required"
}

object JobState {
    const val FETCHING = "fetching"
    const val PAUSED = "paused"
    const val INSTALLING = "installing"
    const val RESTARTING = "restarting"
    const val SUCCEEDED = "succeeded"
    const val FAILED = "failed"
    const val CANCELLED = "cancelled"

    fun ended(state: String): Boolean = state == SUCCEEDED || state == FAILED || state == CANCELLED
}

/**
 * The agent's event `link.closing`: the last thing it sends on a link that it is about to end of its own accord,
 * with why in `reason`. The phone made the link and the phone ends it, so the app closes its side the moment the
 * event arrives and sends nothing more (`AgentSession`). A reason the app does not know ends the link all the same.
 */
object LinkClosing {
    const val EVENT = "link.closing"

    /** The agent's process ends for an update of the agent, and its new version starts in its place. */
    const val RESTARTING = "restarting"

    /** The agent's process ends, and nothing starts it again. */
    const val STOPPING = "stopping"

    /** This phone's pairing was ended on the car. */
    const val FORGOTTEN = "forgotten"

    /** The same phone opened a newer link. */
    const val REPLACED = "replaced"

    /** The agent heard nothing from the phone for longer than a link may stay silent. */
    const val SILENT = "silent"
}

/**
 * Where a job stands: the parameters of `job.progress`, and the `job` object of `job.start` and `job.status`,
 * which adds [seq] and, once the job has ended, [result].
 */
@Serializable
data class JobSnapshot(
    val planId: String,
    val state: String,
    /** The step being worked on, counted from 1, and how many the plan has. */
    val step: Int = 0,
    val steps: Int = 0,
    /** The package of that step. */
    val id: String = "",
    val phase: String? = null,
    /** Progress within the phase, for `fetch` and `files`. */
    val bytes: Long? = null,
    val total: Long? = null,
    /** The number of the job's latest event. */
    val seq: Long = 0,
    val result: JobResult? = null,
) {
    val ended: Boolean get() = JobState.ended(state)
}

/** The parameters of `job.log`: text from the agent or from a hook script. */
@Serializable
data class JobLog(
    val planId: String,
    val step: Int = 0,
    /** `agent`, or the hook that wrote the text. */
    val source: String = "agent",
    val text: String = "",
)

@Serializable
data class StepError(val code: String, val message: String = "")

/** How one step of a job came out: `done`, `failed`, `broken` or `skipped`. */
@Serializable
data class StepResult(
    val action: String,
    val id: String,
    val version: String = "",
    val oldVersion: String? = null,
    val result: String,
    val error: StepError? = null,
)

/** The parameters of `job.done`. */
@Serializable
data class JobResult(
    val planId: String,
    val state: String,
    val steps: List<StepResult> = emptyList(),
    /** What the steps that are done need now. */
    val restart: String = RestartKind.NONE,
)

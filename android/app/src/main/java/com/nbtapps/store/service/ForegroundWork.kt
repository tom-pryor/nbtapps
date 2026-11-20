// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.service

import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.logs.LogPullState
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.JobState

/**
 * A reason for [JobService] to run: something the app is doing with a head unit that has to go on with the app out of
 * sight. Each says what the notification shows when it is the main one, and the line it adds when another is.
 */
sealed interface ForegroundReason {
    /** The notification's title, as the main reason. */
    val title: String

    /** The line under the title, as the main reason, if any. */
    val text: String? get() = null

    /** The bar, as the main reason, if any. */
    val progress: NoticeProgress? get() = null

    /** The line this adds below the main reason's. */
    val line: String get() = title

    /** Whether the phone is kept from dozing for it: a transfer that the head unit waits on. */
    val awake: Boolean get() = false

    /** A job that needs the phone. */
    data class Job(val view: JobView) : ForegroundReason {
        override val title: String get() = AgentText.state(view.job.state)

        override val text: String
            get() {
                val job = view.job
                val step = if (job.steps > 0) "Step ${job.step} of ${job.steps}: " else ""
                return step + AgentText.phase(job.phase) + " · keep the ignition on"
            }

        override val progress: NoticeProgress
            get() {
                val total = view.job.total
                val bytes = view.job.bytes
                return NoticeProgress(if (total != null && bytes != null && total > 0) permille(bytes, total) else null)
            }

        // And while the app waits to connect again after the agent restarted: a phone that dozes sits out the wait for
        // far longer than it is meant to last.
        override val awake: Boolean get() = view.job.state == JobState.FETCHING || view.reconnecting
    }

    /** A pull of logs. */
    data class LogPull(val pull: LogPullState.Pulling) : ForegroundReason {
        override val title: String get() = "Getting logs from the head unit"
        override val text: String get() = "File ${pull.file} of ${pull.files} · keep the ignition on"
        override val progress: NoticeProgress get() = NoticeProgress(if (pull.total > 0) permille(pull.bytes, pull.total) else null)
        override val awake: Boolean get() = true
    }

    /** A terminal on the head unit called [headUnitName], open, or being opened. */
    data class Terminal(val headUnitName: String, val opening: Boolean) : ForegroundReason {
        override val title: String get() = if (opening) "Opening a terminal on $headUnitName" else "Terminal open on $headUnitName"
    }

    /**
     * The SSH port for other apps, on for the head unit called [headUnitName] ([SshPortState.On]), or being switched on
     * ([SshPortState.Checking], with no [port] yet). It brings the notification's action that turns it off.
     */
    data class Port(val headUnitName: String, val port: Int?, val connections: Int, val paused: Boolean) : ForegroundReason {
        override val title: String get() = "SSH access on for $headUnitName"

        override val text: String get() = if (port == null) "Checking the way to the head unit…" else "Port $port · ${state()}"

        override val line: String get() = if (port == null) "SSH access · checking" else "SSH access on · ${state()}"

        private fun state(): String = if (paused) "paused" else connectionsText(connections)
    }
}

/** "no connections", "1 connection", "2 connections". */
fun connectionsText(count: Int): String = when (count) {
    0 -> "no connections"
    1 -> "1 connection"
    else -> "$count connections"
}

/** A notification's bar: [permille] of 1000, or one that moves with no end where it is null. */
data class NoticeProgress(val permille: Int?)

/** What [JobService]'s notification shows: a [title], the [lines] under it, a bar, and whether the phone is kept awake. */
data class ForegroundNotice(
    val title: String,
    val lines: List<String>,
    val progress: NoticeProgress?,
    val awake: Boolean,
    /** The label of the action that turns the SSH port off, while it is on; null with no port. */
    val portAction: String? = null,
)

/** What [JobService] runs for, and what its notification says. */
object ForegroundWork {
    /**
     * Whether the notification is to be posted again now: it was last posted while notifications were not allowed
     * ([unseen]), when Android drops it, and they are allowed now ([allowed]), as once Android's question has been
     * answered Allow, or they were allowed in the phone's settings.
     */
    fun postAgain(unseen: Boolean, allowed: Boolean): Boolean = unseen && allowed

    /**
     * The reasons to run now, the main one first: a job that needs the phone, a pull of logs, a terminal open or being
     * opened on a head unit, the SSH port on or being switched on; the head units' names are looked up in [headUnits].
     */
    fun reasons(
        job: JobView?,
        pull: LogPullState?,
        terminal: TerminalState,
        headUnits: List<HeadUnit>,
        port: SshPortState = SshPortState.Off(),
    ): List<ForegroundReason> = buildList {
        job?.takeIf { it.needsPhone }?.let { add(ForegroundReason.Job(it)) }
        (pull as? LogPullState.Pulling)?.let { add(ForegroundReason.LogPull(it)) }
        if (terminal.active) {
            add(ForegroundReason.Terminal(nameOf(headUnits, terminal.headUnitId), opening = terminal is TerminalState.Connecting))
        }
        when (port) {
            is SshPortState.Checking -> add(ForegroundReason.Port(nameOf(headUnits, port.headUnitId), null, 0, false))
            is SshPortState.On -> add(ForegroundReason.Port(nameOf(headUnits, port.headUnitId), port.port, port.connections, port.paused))
            is SshPortState.Off -> {}
        }
    }

    private fun nameOf(headUnits: List<HeadUnit>, id: String?): String = headUnits.firstOrNull { it.id == id }?.name ?: "the head unit"

    /**
     * The head unit whose terminal a tap on the notification opens: the one a terminal is open, or being opened, on,
     * whatever else the notification is for. Null where there is none: the tap brings the app up as it was.
     */
    fun terminalToOpen(terminal: TerminalState): String? = terminal.headUnitId.takeIf { terminal.active }

    /**
     * The head unit whose "SSH from other apps" a tap on the notification opens: the one the port is on, or being
     * switched on, for, where no terminal is open or being opened ([terminalToOpen] comes first). Null otherwise.
     */
    fun sshAccessToOpen(terminal: TerminalState, port: SshPortState): String? =
        port.headUnitId.takeIf { port.active && !terminal.active }

    /**
     * The notification for [reasons]: the first one's title, line and bar, and below them a line for each of the
     * others; with the SSH port among them, the action that turns it off, "Turn off" where the port is the main reason
     * and "Turn off SSH port" beside another. Null where there is no reason: the service stops.
     */
    fun notice(reasons: List<ForegroundReason>): ForegroundNotice? {
        val main = reasons.firstOrNull() ?: return null
        return ForegroundNotice(
            title = main.title,
            lines = listOfNotNull(main.text) + reasons.drop(1).map { it.line },
            progress = main.progress,
            awake = reasons.any { it.awake },
            portAction = when {
                main is ForegroundReason.Port -> TURN_OFF
                reasons.any { it is ForegroundReason.Port } -> TURN_OFF_PORT
                else -> null
            },
        )
    }

    const val TURN_OFF = "Turn off"
    const val TURN_OFF_PORT = "Turn off SSH port"
}

private fun permille(part: Long, whole: Long): Int = (part * 1000 / whole).toInt().coerceIn(0, 1000)

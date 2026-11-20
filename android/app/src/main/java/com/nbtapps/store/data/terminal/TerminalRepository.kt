// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.terminal

import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobKeeper
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.monotonicMillis
import com.nbtapps.store.data.ssh.RelayError
import com.nbtapps.store.data.ssh.ShellEnd
import com.nbtapps.store.data.ssh.SshIdentity
import com.nbtapps.store.data.ssh.SshShell
import com.nbtapps.store.data.ssh.SshShellException
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.ssh.SshTunnelException
import com.nbtapps.store.data.ssh.TunnelFailure
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.ChannelCloseReason
import com.nbtapps.store.model.ChannelError
import java.io.IOException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** How the terminal stands. Each state but [Idle] is of the head unit [headUnitId], by this app's id. */
sealed interface TerminalState {
    val headUnitId: String?

    /** A shell is being opened or is open: the link is held for it, and the foreground service runs for it. */
    val active: Boolean get() = this is Connecting || this is Open

    /** No terminal has been opened in this run of the app. */
    data object Idle : TerminalState {
        override val headUnitId: String? get() = null
    }

    /** The link is being made, if there was none, and then the shell opened. */
    data class Connecting(override val headUnitId: String) : TerminalState

    /** The shell is open. [held] while a job runs on the head unit, which holds every channel until it has ended. */
    data class Open(override val headUnitId: String, val held: Boolean = false) : TerminalState

    /** The shell that was open has ended, as [how] says. */
    data class Ended(override val headUnitId: String, val how: TerminalEnd) : TerminalState

    /** No shell could be had, or one that was open ended in a fault: [reason] is for the user. */
    data class Failed(override val headUnitId: String, val reason: String) : TerminalState

    /** The head unit's agent has no channels: it has to be updated first. */
    data class Unsupported(override val headUnitId: String) : TerminalState

    /** The SSH package is not installed on the head unit. */
    data class NoPackage(override val headUnitId: String) : TerminalState

    /** SSH access is switched off on the head unit. */
    data class Off(override val headUnitId: String) : TerminalState
}

/** How a shell that was open ended. */
sealed interface TerminalEnd {
    /** The shell exited, with [status]. */
    data class Exited(val status: Int) : TerminalEnd

    /** The head unit's SSH server closed the connection, and gave no exit status. */
    data object ServerClosed : TerminalEnd

    /** SSH access was switched off on the head unit. */
    data object SwitchedOff : TerminalEnd

    /** A job installed, upgraded or removed the SSH package. */
    data object PackageChanged : TerminalEnd

    /** The link to the head unit ended, for [reason], which is for the user. */
    data class LinkLost(val reason: String) : TerminalEnd

    /** Nothing was sent or received for [TerminalRepository.IDLE_MS]. */
    data object Idle : TerminalEnd

    /** The user closed it: [TerminalRepository.close]. */
    data object ByUser : TerminalEnd
}

/** The words of the app's own lines in a terminal ([TerminalBuffer.mark]). */
object TerminalText {
    /** Above a shell that opens below the lines of one before it. */
    const val NEW_SHELL = "New shell"

    /** Below the lines of a shell that ended as [how] says. */
    fun ended(how: TerminalEnd): String = when (how) {
        is TerminalEnd.Exited -> "The shell ended, with exit status ${how.status}"
        TerminalEnd.ServerClosed -> "The head unit's SSH server closed the connection"
        TerminalEnd.SwitchedOff -> "SSH access was switched off on the head unit"
        TerminalEnd.PackageChanged -> "The SSH package was changed on the head unit"
        is TerminalEnd.LinkLost -> "The link to the head unit ended: ${how.reason}"
        TerminalEnd.Idle -> "Closed after ${TerminalRepository.IDLE_MS / 60_000} minutes with nothing sent or received"
        TerminalEnd.ByUser -> "Closed"
    }
}

/** What a terminal needs of the app's link to a head unit: [JobRepository]'s ([JobTerminalLink]), or a test's. */
interface TerminalLink {
    val connection: StateFlow<ConnectionState>

    /** A connect as the user's is: [JobRepository.connect]. */
    suspend fun connect(headUnitId: String): Result<Unit>

    /** [block] with the link to [headUnitId], as a use of it: [JobRepository.usingSession]. */
    suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T>

    /**
     * Why [session] ended, settled, where the link's own [reason] is what the shell was told: [JobRepository.endReason],
     * what the head unit's page shows too. It may wait a moment.
     */
    suspend fun endReason(session: AgentSession, reason: String): String = reason
}

/** The app's link, [JobRepository]'s, as a terminal uses it. */
class JobTerminalLink(private val jobs: JobRepository) : TerminalLink {
    override val connection: StateFlow<ConnectionState> get() = jobs.connection

    override suspend fun connect(headUnitId: String): Result<Unit> = jobs.connect(headUnitId)

    override suspend fun <T> usingSession(headUnitId: String, block: suspend (AgentSession) -> T): Result<T> =
        jobs.usingSession(headUnitId, block)

    override suspend fun endReason(session: AgentSession, reason: String): String = jobs.endReason(session, reason)
}

/** A shell on a head unit as a terminal uses one: an [SshShell] over its [SshTunnel] ([SshShellOpener]), or a test's. */
interface TerminalShell {
    /** The tunnel's channel: open, paused while a job runs on the head unit, or closed. */
    val channel: StateFlow<ChannelState>

    /** Completes once the shell has ended and nothing of it is left: [SshShell.ended]. */
    val ended: Deferred<ShellEnd>

    /** [SshShell.read]. */
    suspend fun read(): ByteArray?

    /** [SshShell.write]. */
    suspend fun write(bytes: ByteArray)

    /** [SshShell.resize]. */
    suspend fun resize(columns: Int, rows: Int)

    /** [SshShell.close]. */
    suspend fun close(): ShellEnd
}

/** Opens a [TerminalShell] over a link. */
fun interface ShellOpener {
    /**
     * Opens a shell on a pty of [columns] by [rows] over [session], whose end is watched in [scope]. A caller that is
     * cancelled, and a failure, leave nothing open.
     *
     * @throws SshTunnelException, [SshShellException] or another [IOException], as the shell could not be had.
     */
    suspend fun open(session: AgentSession, columns: Int, rows: Int, scope: CoroutineScope): TerminalShell
}

/**
 * The shell of the head unit's SSH server, through the SSH package's relay: a tunnel with the app's own key ([identity])
 * and no other, then a shell logged in as [user] with it. [user] is `root` on a head unit; the bench's server logs in
 * the user it runs as.
 */
class SshShellOpener(private val identity: SshIdentity, private val user: String = SshShell.ROOT) : ShellOpener {
    override suspend fun open(session: AgentSession, columns: Int, rows: Int, scope: CoroutineScope): TerminalShell {
        val key = identity.key()
        val tunnel = SshTunnel.open(session, listOf(key.publicKeyLine()))
        val shell = SshShell.open(tunnel, key, user, columns, rows, scope)
        return object : TerminalShell {
            override val channel: StateFlow<ChannelState> get() = tunnel.state
            override val ended: Deferred<ShellEnd> get() = shell.ended

            override suspend fun read(): ByteArray? = shell.read()

            override suspend fun write(bytes: ByteArray) = shell.write(bytes)

            override suspend fun resize(columns: Int, rows: Int) = shell.resize(columns, rows)

            override suspend fun close(): ShellEnd = shell.close()
        }
    }
}

/**
 * The app's terminal: one shell at a time on one head unit's SSH server, and the lines it has written ([lines]). It
 * lives as long as the process, as [JobRepository] does, and nothing of it is kept beyond.
 *
 * [open] connects first where the app has no link to that head unit, as a connect of the user's is made, and then
 * opens a tunnel with the app's own key and a shell over it. From then until the shell has ended, the shell is a use of
 * the link ([JobRepository.usingSession]), run in the app's scope and not a screen's: leaving the screen, or the app,
 * drops neither the link nor the shell, and the foreground service runs for it ([active][TerminalState.active]).
 *
 * While the shell is open, what it writes goes into a [TerminalBuffer] as it comes: the reader never waits for a screen,
 * and the shell's own hold-back is what paces the server. The lines are given out at most every [PUBLISH_EVERY_MS], so
 * that a large output does not redraw a screen for every piece. A shell with nothing sent and nothing received for
 * [IDLE_MS] is closed, before the server's own limit of 30 minutes ends the connection.
 *
 * However a shell ends, its lines stay, with a line of the app's own that says how ([TerminalText.ended]); the next
 * [open] on the same head unit opens a new shell below them, after another ([TerminalText.NEW_SHELL]). [clear] empties
 * them but the line the shell is writing. An [open] on another head unit empties them whole first: the lines are of
 * one head unit's shells.
 */
class TerminalRepository(
    private val link: TerminalLink,
    private val opener: ShellOpener,
    private val keeper: JobKeeper,
    private val scope: CoroutineScope,
    /**
     * A clock in milliseconds that only goes forward, and goes on while the phone sleeps: what the idle limit is measured
     * on, as the server measures its own. `SystemClock.elapsedRealtime` in the app.
     */
    private val clock: () -> Long = ::monotonicMillis,
) {
    private val buffer = TerminalBuffer()

    private val _state = MutableStateFlow<TerminalState>(TerminalState.Idle)

    /** How the terminal stands. */
    val state: StateFlow<TerminalState> = _state.asStateFlow()

    private val _lines = MutableStateFlow(buffer.snapshot())

    /** The lines, given out at most every [PUBLISH_EVERY_MS]: a screen that is slow, or gone, holds nothing up. */
    val lines: StateFlow<TerminalSnapshot> = _lines.asStateFlow()

    /** Guards [running], [boundTo] and each [Run]'s fields that say so. */
    private val lock = Any()

    /** The shell being opened or open, if there is one. */
    private var running: Run? = null

    /** The head unit whose shells the lines are of. */
    private var boundTo: String? = null

    /** The pty's size the screen wants, in characters: what a shell opens with, and what [resize] asks for. */
    private val size = MutableStateFlow(Size(DEFAULT_COLUMNS, DEFAULT_ROWS))

    /** Says that the lines changed, to [publish]. */
    private val changes = Channel<Unit>(Channel.CONFLATED)

    private data class Size(val columns: Int, val rows: Int)

    /** What goes to the shell, in order: what the user typed, and a new size. */
    private sealed interface Outgoing {
        class Bytes(val bytes: ByteArray) : Outgoing

        data class Resize(val size: Size) : Outgoing
    }

    /** One shell, from [open] to its end. */
    private class Run(val headUnitId: String) {
        lateinit var job: Job

        /** The shell, once it is open. Guarded by [lock]. */
        var shell: TerminalShell? = null

        /** Why the app ends the shell, once it does. Guarded by [lock]. */
        var closing: TerminalEnd? = null

        /** Whether the shell was shown open, and its lines are to be marked with its end. */
        @Volatile
        var opened = false

        /** When something was last sent or received, on the clock. */
        @Volatile
        var heard = 0L

        /** The link's own reason, where the shell, or its opening, ended with the link: settled once the use is over. */
        @Volatile
        var linkLost: String? = null

        val outgoing = Channel<Outgoing>(Channel.UNLIMITED)
    }

    init {
        scope.launch { publish() }
    }

    /**
     * Opens a shell on a pty of [columns] by [rows] on the head unit [headUnitId], in the app's scope: see the class.
     * How it goes is in [state]. Nothing is done while a shell is being opened or is open.
     */
    fun open(headUnitId: String, columns: Int, rows: Int) {
        synchronized(lock) {
            if (running != null) return
            if (boundTo != headUnitId) {
                buffer.reset()
                boundTo = headUnitId
            }
            if (columns > 0 && rows > 0) size.value = Size(columns, rows)
            val run = Run(headUnitId)
            running = run
            _state.value = TerminalState.Connecting(headUnitId)
            // Run to its end even where it is given up at once ([close]): the end is what lets the next one begin. Its
            // job is set before [close] can look at it, which takes the lock.
            run.job = scope.launch(start = CoroutineStart.ATOMIC) { lifetime(run) }
        }
        changed()
        // While the app is in front of the user, as it is when the user opens a terminal: the service may be started.
        keeper.jobRunning()
    }

    /** Sends [bytes] to the shell, as typed, in order. Nothing is sent where no shell is open. */
    fun send(bytes: ByteArray) {
        if (bytes.isEmpty()) return
        synchronized(lock) {
            val run = running?.takeIf { it.shell != null && it.closing == null } ?: return
            run.heard = clock()
            run.outgoing.trySend(Outgoing.Bytes(bytes.copyOf()))
        }
    }

    /**
     * The screen's size for the pty, in characters. A layout gives many in a row: the shell is told the last of them
     * once they have stopped for [RESIZE_AFTER_MS], and the next shell opens with it.
     */
    fun resize(columns: Int, rows: Int) {
        if (columns > 0 && rows > 0) size.value = Size(columns, rows)
    }

    /** Ends the shell, or gives up opening one. Its lines stay. Nothing is done where there is none. */
    fun close() {
        val shell = synchronized(lock) {
            val run = running ?: return
            if (run.closing == null) run.closing = TerminalEnd.ByUser
            // Still opening: given up.
            run.shell.also { if (it == null) run.job.cancel() }
        } ?: return
        scope.launch { shell.close() }
    }

    /**
     * Empties the lines but the one being written ([TerminalBuffer.clear]): an open shell's prompt stays, and what it
     * writes next goes on in that line. With no shell open nothing is left.
     */
    fun clear() {
        buffer.clear()
        changed()
    }

    /** The whole text of the lines, as they are now: what Copy copies. */
    fun text(): String = buffer.text()

    private fun changed() {
        changes.trySend(Unit)
    }

    /** Gives the lines out after each change, at most every [PUBLISH_EVERY_MS], for as long as the repository lives. */
    private suspend fun publish() {
        for (change in changes) {
            _lines.value = buffer.snapshot()
            delay(PUBLISH_EVERY_MS)
        }
    }

    /** What [open] began: the shell from its start to its end, and the state it ends in. */
    private suspend fun lifetime(run: Run) {
        var end: TerminalState = TerminalState.Ended(run.headUnitId, TerminalEnd.ByUser)
        try {
            end = attempt(run)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // Nothing that goes wrong here may end the app.
            end = TerminalState.Failed(run.headUnitId, "The terminal ran into a fault: ${e.message ?: e.javaClass.simpleName}")
        } finally {
            finish(run, end)
        }
    }

    private suspend fun attempt(run: Run): TerminalState {
        // Given up before it began: nothing is done, not even a connect.
        currentCoroutineContext().ensureActive()
        val id = run.headUnitId
        if (connectedTo(id) == null) {
            link.connect(id).exceptionOrNull()?.let { return TerminalState.Failed(id, it.message ?: CONNECT_FAILED) }
        }
        val connected = connectedTo(id) ?: return TerminalState.Failed(id, NOT_CONNECTED)
        if (!connected.channels) return TerminalState.Unsupported(id)
        var used: AgentSession? = null
        val state = link.usingSession(id) { session ->
            used = session
            over(run, session)
        }.getOrElse { TerminalState.Failed(id, it.message ?: NOT_CONNECTED) }
        // A lost link's reason, settled as the head unit's page has it, once the link is no longer held for the shell.
        val lost = run.linkLost ?: return state
        val session = used ?: return state
        val settled = link.endReason(session, lost)
        return when {
            settled == lost -> state
            state is TerminalState.Ended && state.how is TerminalEnd.LinkLost -> TerminalState.Ended(id, TerminalEnd.LinkLost(settled))
            state is TerminalState.Failed -> TerminalState.Failed(id, settled)
            else -> state
        }
    }

    /** Notes where the shell, or its opening, ended with the link ([end]): see [Run.linkLost]. */
    private fun lostWith(run: Run, end: ChannelEnd?) {
        if (end is ChannelEnd.LinkLost) run.linkLost = end.reason
    }

    private fun connectedTo(headUnitId: String): ConnectionState.Connected? =
        (link.connection.value as? ConnectionState.Connected)?.takeIf { it.headUnitId == headUnitId }

    /** Opens the shell over [session] and serves it to its end, as a use of the link. */
    private suspend fun over(run: Run, session: AgentSession): TerminalState {
        val id = run.headUnitId
        val opened = size.value
        val shell = try {
            opener.open(session, opened.columns, opened.rows, scope)
        } catch (e: SshTunnelException) {
            lostWith(run, (e.failure as? TunnelFailure.Closed)?.end)
            return refused(id, e.failure)
        } catch (e: SshShellException) {
            lostWith(run, (e.end as? ShellEnd.TunnelCut)?.end)
            return notStarted(id, e.end)
        } catch (e: IOException) {
            return TerminalState.Failed(id, e.message ?: "The terminal couldn't be opened")
        }
        val closing = synchronized(lock) {
            run.shell = shell
            run.closing
        }
        if (closing != null) {
            // Closed while it opened.
            withContext(NonCancellable) { shell.close() }
            return TerminalState.Ended(id, closing)
        }
        synchronized(lock) {
            if (buffer.snapshot().lines.isNotEmpty()) buffer.mark(TerminalText.NEW_SHELL)
            run.opened = true
            _state.value = TerminalState.Open(id, held = shell.channel.value == ChannelState.Paused)
        }
        changed()
        val end = serve(run, shell, opened)
        lostWith(run, (end as? ShellEnd.TunnelCut)?.end)
        return ended(id, end, synchronized(lock) { run.closing })
    }

    /**
     * Reads what the shell writes into the buffer until it has ended, and meanwhile sends what is typed and the size,
     * follows the channel's pause, and closes the shell once it has been idle too long. Returns how it ended, with
     * nothing of it left.
     */
    private suspend fun serve(run: Run, shell: TerminalShell, opened: Size): ShellEnd = try {
        coroutineScope {
            run.heard = clock()
            val helpers = listOf(
                launch { forward(run, shell) },
                launch { follow(run, shell) },
                launch { sizes(run, opened) },
                launch { idle(run, shell) },
            )
            while (true) {
                val piece = shell.read() ?: break
                buffer.write(piece)
                run.heard = clock()
                changed()
            }
            val end = shell.ended.await()
            helpers.forEach { it.cancel() }
            end
        }
    } catch (e: CancellationException) {
        // The app's scope ended: so does the shell.
        withContext(NonCancellable) { shell.close() }
        throw e
    }

    /** Sends what is typed, and each new size, one after another: a send that waits for the head unit holds the rest. */
    private suspend fun forward(run: Run, shell: TerminalShell) {
        for (out in run.outgoing) {
            when (out) {
                is Outgoing.Bytes -> shell.write(out.bytes)
                is Outgoing.Resize -> shell.resize(out.size.columns, out.size.rows)
            }
        }
    }

    /** Shows the shell as held while its channel is paused. */
    private suspend fun follow(run: Run, shell: TerminalShell) {
        shell.channel.collect { now ->
            if (now is ChannelState.Closed) return@collect
            val held = now == ChannelState.Paused
            _state.update { shown -> if (shown is TerminalState.Open && shown.headUnitId == run.headUnitId) shown.copy(held = held) else shown }
        }
    }

    /** Tells the shell the screen's size once it has stopped changing for [RESIZE_AFTER_MS]. */
    private suspend fun sizes(run: Run, opened: Size) {
        var told = opened
        size.collectLatest { wanted ->
            delay(RESIZE_AFTER_MS)
            if (wanted != told) {
                told = wanted
                run.outgoing.trySend(Outgoing.Resize(wanted))
            }
        }
    }

    /**
     * Closes the shell once nothing has been sent or received for [IDLE_MS] on the clock. The clock is looked at every
     * [IDLE_CHECK_MS] at most, since a wait does not count the time the phone slept.
     */
    private suspend fun idle(run: Run, shell: TerminalShell) {
        while (true) {
            val left = run.heard + IDLE_MS - clock()
            if (left <= 0) break
            delay(minOf(left, IDLE_CHECK_MS))
        }
        synchronized(lock) { if (run.closing == null) run.closing = TerminalEnd.Idle }
        shell.close()
    }

    /** The run is over, in [end]: its lines are marked where a shell was open, and the next [open] may begin. */
    private fun finish(run: Run, end: TerminalState) {
        run.outgoing.close()
        synchronized(lock) {
            if (run.opened) {
                buffer.finish()
                buffer.mark(
                    when (end) {
                        is TerminalState.Ended -> TerminalText.ended(end.how)
                        is TerminalState.Failed -> end.reason
                        else -> TerminalText.ended(TerminalEnd.ServerClosed)
                    },
                )
            }
            _state.value = end
            running = null
        }
        changed()
    }

    /** What the agent's or the relay's refusal of the tunnel is shown as. */
    private fun refused(id: String, failure: TunnelFailure): TerminalState = when (failure) {
        is TunnelFailure.Refused -> when (failure.code) {
            ChannelError.OFF -> TerminalState.Off(id)
            ChannelError.NOT_FOUND -> TerminalState.NoPackage(id)
            ChannelError.UNSUPPORTED -> TerminalState.Unsupported(id)
            ChannelError.BUSY -> TerminalState.Failed(id, "The head unit is busy with a job. Open the terminal once it has finished.")
            ChannelError.UNAVAILABLE -> TerminalState.Failed(id, "The SSH package on the head unit didn't start")
            ChannelError.TOO_MANY -> TerminalState.Failed(id, "The link to the head unit has as many connections open as it can take")
            else -> TerminalState.Failed(id, AgentText.request(failure.code))
        }
        is TunnelFailure.Relay -> TerminalState.Failed(
            id,
            when (failure.code) {
                RelayError.TOO_MANY -> "The head unit's SSH server has as many connections as it takes"
                RelayError.FAILED -> "The head unit's SSH server couldn't be started"
                RelayError.BAD_KEYS -> "The SSH package on the head unit didn't take this phone's key"
                else -> "The SSH package on the head unit refused the connection (${failure.code})"
            },
        )
        is TunnelFailure.BadAnswer -> TerminalState.Failed(id, "The SSH package on the head unit sent an answer this app can't read")
        is TunnelFailure.Closed -> cutBefore(id, failure.end)
        is TunnelFailure.NoAnswer -> TerminalState.Failed(
            id,
            if (failure.toKeys) "The SSH package on the head unit didn't answer in time" else "The head unit didn't open SSH in time",
        )
    }

    /** What a shell that did not start is shown as. */
    private fun notStarted(id: String, end: ShellEnd): TerminalState = when (end) {
        ShellEnd.NoPty -> TerminalState.Failed(id, "The head unit has no pty driver running, so it can't give a terminal")
        ShellEnd.HostKeyRefused -> TerminalState.Failed(id, "The head unit's SSH server isn't the one the agent named, so nothing was sent to it")
        ShellEnd.LoginRefused -> TerminalState.Failed(id, "The head unit's SSH server didn't take this phone's key")
        is ShellEnd.Failed -> TerminalState.Failed(id, end.reason)
        is ShellEnd.TunnelCut -> cutBefore(id, end.end)
        else -> TerminalState.Failed(id, "The shell didn't open")
    }

    /** What a channel that ended before the shell was open is shown as. */
    private fun cutBefore(id: String, end: ChannelEnd): TerminalState = when (end) {
        is ChannelEnd.ByAgent -> when (end.reason) {
            ChannelCloseReason.OFF -> TerminalState.Off(id)
            ChannelCloseReason.PACKAGE_CHANGED -> TerminalState.Failed(id, "The SSH package was being changed on the head unit. Try again once the job has finished.")
            else -> TerminalState.Failed(id, "The head unit closed the SSH connection before it was up")
        }
        ChannelEnd.ByApp -> TerminalState.Failed(id, "The SSH connection was closed before it was up")
        is ChannelEnd.AgentFault -> TerminalState.Failed(id, "The head unit broke the channel's rules: ${end.what}")
        is ChannelEnd.LinkLost -> TerminalState.Failed(id, end.reason)
    }

    /** What the end of a shell that was open is shown as. [closing] is why the app closed it, if it did. */
    private fun ended(id: String, end: ShellEnd, closing: TerminalEnd?): TerminalState = when (end) {
        is ShellEnd.Exited -> TerminalState.Ended(id, TerminalEnd.Exited(end.status))
        ShellEnd.ServerClosed -> TerminalState.Ended(id, TerminalEnd.ServerClosed)
        ShellEnd.ByApp -> TerminalState.Ended(id, closing ?: TerminalEnd.ByUser)
        is ShellEnd.TunnelCut -> when (val cut = end.end) {
            is ChannelEnd.ByAgent -> when (cut.reason) {
                ChannelCloseReason.OFF -> TerminalState.Ended(id, TerminalEnd.SwitchedOff)
                ChannelCloseReason.PACKAGE_CHANGED -> TerminalState.Ended(id, TerminalEnd.PackageChanged)
                ChannelCloseReason.CLOSED -> TerminalState.Ended(id, TerminalEnd.ServerClosed)
                else -> TerminalState.Failed(id, "The head unit closed the SSH connection (${cut.reason})")
            }
            ChannelEnd.ByApp -> TerminalState.Ended(id, closing ?: TerminalEnd.ByUser)
            is ChannelEnd.LinkLost -> TerminalState.Ended(id, TerminalEnd.LinkLost(cut.reason))
            is ChannelEnd.AgentFault -> TerminalState.Failed(id, "The head unit broke the channel's rules: ${cut.what}")
        }
        is ShellEnd.Failed -> TerminalState.Failed(id, end.reason)
        else -> TerminalState.Failed(id, "The shell ended")
    }

    companion object {
        /**
         * A shell with nothing sent and nothing received for this long is closed, in milliseconds: a minute before the
         * head unit's SSH server ends a connection that has been idle for 30, so that the app's own end comes first.
         */
        const val IDLE_MS = 29 * 60_000L

        /** How often the idle limit is looked at, at most, in milliseconds. */
        const val IDLE_CHECK_MS = 60_000L

        /** The lines are given out at most this often, in milliseconds: about 20 times a second. */
        const val PUBLISH_EVERY_MS = 50L

        /** A size is told to the shell once the screen's size has not changed for this long, in milliseconds. */
        const val RESIZE_AFTER_MS = 250L

        /** The pty's size until a screen gives one. */
        const val DEFAULT_COLUMNS = 80
        const val DEFAULT_ROWS = 24

        private const val NOT_CONNECTED = "Not connected to the head unit"
        private const val CONNECT_FAILED = "The head unit couldn't be reached"
    }
}

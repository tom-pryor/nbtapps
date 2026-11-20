// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.jcraft.jsch.ChannelShell
import com.jcraft.jsch.HostKey
import com.jcraft.jsch.HostKeyRepository
import com.jcraft.jsch.Identity
import com.jcraft.jsch.JSch
import com.jcraft.jsch.JSchException
import com.jcraft.jsch.JSchHostKeyException
import com.jcraft.jsch.Proxy
import com.jcraft.jsch.Session
import com.jcraft.jsch.SocketFactory
import com.jcraft.jsch.UserInfo
import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.model.ChannelCloseReason
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ThreadFactory
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.trySendBlocking
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runInterruptible
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

/** How a shell ended, or why it never began. */
sealed interface ShellEnd {
    /** The shell exited, with [status]. */
    data class Exited(val status: Int) : ShellEnd

    /** The server closed the shell, or the whole connection, and gave no exit status. */
    data object ServerClosed : ShellEnd

    /**
     * The tunnel was cut, as [end] says: the switch turned off ([ChannelEnd.ByAgent] with `off`), the package
     * changed (`package-changed`), the link lost ([ChannelEnd.LinkLost]).
     */
    data class TunnelCut(val end: ChannelEnd) : ShellEnd

    /** The app closed it: [SshShell.close]. */
    data object ByApp : ShellEnd

    /** The relay's answer said there is no pty to be had on the head unit, so no shell was tried. */
    data object NoPty : ShellEnd

    /** The server's host key is not one the tunnel gave: nothing was sent for a login. */
    data object HostKeyRefused : ShellEnd

    /** The server did not take the key. */
    data object LoginRefused : ShellEnd

    /** Something else went wrong, as [reason] says, in words for the user. */
    data class Failed(val reason: String) : ShellEnd
}

/** What [SshShell.open] fails with. */
class SshShellException(val end: ShellEnd) : IOException("The shell did not open: $end")

/**
 * A shell on a pty on the head unit, over an [SshTunnel]: an SSH session with JSch (`com.github.mwiede:jsch`) that
 * speaks over the tunnel's two streams, through JSch's [Proxy], with no socket. The server's host key has to be one of
 * the tunnel's ([SshTunnel.hostKeys], given over the paired link), and any other is refused before anything is sent
 * for a login. The login is with an [SshKey], as a JSch [Identity] that signs with Bouncy Castle, so the private key is
 * never encoded for JSch. The pty's type is [TERM], `dumb`, and its size what [open] and [resize] give.
 *
 * What JSch does not do by itself over streams is done here: [open] has a time limit of its own; a cut tunnel is
 * noticed by the end of its stream, not by a write, which JSch takes silently after a cut; and the shell's input is
 * never closed while the shell runs, since the server closes a session's pty when the client's input ends.
 *
 * The output comes as it comes ([read]), and [ended] says how the shell ended. Once it has ended, however it ended,
 * the tunnel is closed and JSch's threads and the shell's coroutine are gone.
 */
class SshShell private constructor(
    private val tunnel: SshTunnel,
    private val session: Session,
    private val channel: ChannelShell,
    private val toShell: OutputStream,
    private val threads: SessionThreads,
    private val sink: OutputSink,
) {
    private val _ended = CompletableDeferred<ShellEnd>()

    /**
     * Completes once the shell has ended and nothing of it is left, with how it ended: [ShellEnd.Exited],
     * [ShellEnd.ServerClosed], [ShellEnd.TunnelCut], [ShellEnd.Failed] or [ShellEnd.ByApp].
     */
    val ended: Deferred<ShellEnd> get() = _ended

    private val writing = Mutex()

    @Volatile
    private var byApp = false
    private val wake = CompletableDeferred<Unit>()

    /**
     * The next of the shell's output, as it came; or null once the shell has ended and all of its output has been
     * read ([ended] says how). What came and was not read is dropped when the app closes the shell. One caller at a
     * time; one that is slow holds the server back.
     */
    suspend fun read(): ByteArray? = sink.output.receiveCatching().getOrNull()

    /** Sends [bytes] to the shell, as typed. Nothing happens once it has ended. */
    suspend fun write(bytes: ByteArray) {
        if (bytes.isEmpty() || _ended.isCompleted) return
        writing.withLock {
            try {
                runInterruptible(Dispatchers.IO) {
                    toShell.write(bytes)
                    toShell.flush()
                }
            } catch (e: IOException) {
                // The shell has ended, or is ending: [ended] says how.
            }
        }
    }

    /** Tells the server the pty's new size, in characters. Nothing happens once the shell has ended. */
    suspend fun resize(columns: Int, rows: Int) {
        if (_ended.isCompleted) return
        writing.withLock {
            try {
                runInterruptible(Dispatchers.IO) { channel.setPtySize(columns, rows, 0, 0) }
            } catch (e: IOException) {
                // As for a write.
            } catch (e: RuntimeException) {
                // JSch wraps what failed in one.
            }
        }
    }

    /** Ends the shell, unless it has ended already, and returns how it ended once nothing of it is left. */
    suspend fun close(): ShellEnd {
        byApp = !_ended.isCompleted
        wake.complete(Unit)
        return _ended.await()
    }

    /** Waits in [scope] for the shell's end, and then makes sure nothing of it is left. */
    @OptIn(DelicateCoroutinesApi::class)
    private fun watch(scope: CoroutineScope) {
        threads.onEnd = { wake.complete(Unit) }
        sink.onClose = { wake.complete(Unit) }
        // Started even in a scope that has ended, so that what is left is tidied all the same.
        scope.launch(Dispatchers.IO, start = CoroutineStart.ATOMIC) {
            var end: ShellEnd = ShellEnd.ByApp
            try {
                coroutineScope {
                    val cut = launch {
                        tunnel.state.first { it is ChannelState.Closed }
                        wake.complete(Unit)
                    }
                    // A thread may have ended, or the output closed, before the hooks were set.
                    if (threads.anyEnded || sink.closed) wake.complete(Unit)
                    wake.await()
                    cut.cancel()
                    end = settle()
                }
            } finally {
                withContext(NonCancellable) { tidy(end) }
            }
        }
    }

    /**
     * How the shell ended, once something has said it is ending. The server's end comes as its output's end, its
     * channel's close or the end of JSch's thread; the exit status comes before the channel's close; and what the
     * tunnel brought before it was cut is still read by JSch, so a moment is given for that.
     */
    private suspend fun settle(): ShellEnd {
        // The output's end alone may come a while before the channel's close: the server sends the exit status
        // between the two.
        while (!byApp && !channel.isClosed && !threads.anyEnded && tunnel.state.value !is ChannelState.Closed) delay(POLL_MS)
        withTimeoutOrNull(SETTLE_MS) {
            while (!byApp && !channel.isClosed && !threads.anyEnded) delay(POLL_MS)
        }
        val tunnelEnd = (tunnel.state.value as? ChannelState.Closed)?.end
        val status = channel.exitStatus
        val failure = threads.failure
        return when {
            byApp -> ShellEnd.ByApp
            status >= 0 -> ShellEnd.Exited(status)
            failure != null -> ShellEnd.Failed(missing(failure))
            tunnelEnd is ChannelEnd.LinkLost || tunnelEnd is ChannelEnd.AgentFault -> ShellEnd.TunnelCut(tunnelEnd)
            tunnelEnd is ChannelEnd.ByAgent && tunnelEnd.reason != ChannelCloseReason.CLOSED -> ShellEnd.TunnelCut(tunnelEnd)
            else -> ShellEnd.ServerClosed
        }
    }

    private suspend fun tidy(end: ShellEnd) {
        if (end == ShellEnd.ByApp) sink.output.cancel()
        teardown(tunnel, session, threads)
        sink.output.close()
        _ended.complete(end)
    }

    companion object {
        /** The pty's type: the app's terminal shows no escape sequences. */
        const val TERM = "dumb"

        /** The user a head unit's server logs in. */
        const val ROOT = "root"

        /**
         * The longest the start may take, in milliseconds, from the tunnel to the shell: the SSH handshake, the login
         * and the shell's channel, a few kilobytes in a few round trips. JSch has no limit of its own over streams.
         */
        const val HANDSHAKE_TIMEOUT_MS = 30_000L

        /** What JSch is told the server is called: the name in its thread's name and its messages. */
        private const val HOST = "headunit"

        private const val SETTLE_MS = 2_000L
        private const val POLL_MS = 20L

        /** Each of JSch's threads is waited for this long at the end, in milliseconds. */
        private const val JOIN_MS = 5_000L

        /**
         * Opens a shell on a pty of [columns] by [rows] over [tunnel], logged in as [user] (`root` on a head unit) with
         * [key]. Its end is watched in [scope]; whatever ends it, nothing of it is left. When the tunnel has no pty
         * ([SshTunnel.pty] false) no shell is tried. A failure, and a caller that is cancelled, close the tunnel.
         *
         * @throws SshShellException with [ShellEnd.NoPty], [ShellEnd.HostKeyRefused], [ShellEnd.LoginRefused],
         *   [ShellEnd.TunnelCut] or [ShellEnd.Failed].
         */
        suspend fun open(
            tunnel: SshTunnel,
            key: SshKey,
            user: String,
            columns: Int,
            rows: Int,
            scope: CoroutineScope,
            handshakeTimeoutMs: Long = HANDSHAKE_TIMEOUT_MS,
        ): SshShell = open(tunnel, tunnel.hostKeys, key, user, columns, rows, scope, handshakeTimeoutMs)

        /** [open], with [hostKeys] in place of the tunnel's: for a test of a host key that is not the server's. */
        internal suspend fun open(
            tunnel: SshTunnel,
            hostKeys: List<String>,
            key: SshKey,
            user: String,
            columns: Int,
            rows: Int,
            scope: CoroutineScope,
            handshakeTimeoutMs: Long,
        ): SshShell {
            if (!tunnel.pty) {
                withContext(NonCancellable) { tunnel.close() }
                throw SshShellException(ShellEnd.NoPty)
            }
            val threads = SessionThreads()
            val sink = OutputSink()
            var session: Session? = null
            val timedOut = AtomicBoolean()
            try {
                val shell = coroutineScope {
                    val timer = launch {
                        delay(handshakeTimeoutMs)
                        timedOut.set(true)
                        tunnel.close()
                    }
                    try {
                        val started = System.nanoTime()
                        runInterruptible(Dispatchers.IO) {
                            val jsch = JSch()
                            jsch.setHostKeyRepository(PinnedHostKeys(hostKeys))
                            jsch.addIdentity(KeyIdentity(key), null)
                            val made = jsch.getSession(user, HOST, 22)
                            session = made
                            made.setProxy(TunnelProxy(tunnel))
                            made.setThreadFactory(threads)
                            made.setDaemonThread(true)
                            made.setConfig("StrictHostKeyChecking", "yes")
                            made.setConfig("PreferredAuthentications", "publickey")
                            made.connect()
                            val shell = made.openChannel("shell") as ChannelShell
                            shell.setPtyType(TERM, columns, rows, 0, 0)
                            shell.setPty(true)
                            // JSch closes it when the channel ends. No input stream is given: JSch would start a
                            // thread to copy it, and close the shell's input when it ended.
                            shell.setOutputStream(sink, false)
                            val left = handshakeTimeoutMs - (System.nanoTime() - started) / 1_000_000
                            shell.connect(left.coerceIn(1L, Int.MAX_VALUE.toLong()).toInt())
                            SshShell(tunnel, made, shell, shell.outputStream, threads, sink)
                        }
                    } finally {
                        timer.cancel()
                    }
                }
                shell.watch(scope)
                return shell
            } catch (e: CancellationException) {
                withContext(NonCancellable) { teardown(tunnel, session, threads) }
                throw e
            } catch (e: Throwable) {
                withContext(NonCancellable) { teardown(tunnel, session, threads) }
                // A caller that gave up: JSch tells the interrupt as a failure of its own.
                currentCoroutineContext().ensureActive()
                // A LinkageError is a class or method this Android lacks: told as a failure, it cannot end the app.
                if (e !is Exception && e !is LinkageError) throw e
                throw SshShellException(failure(e, tunnel, timedOut.get(), threads))
            }
        }

        /** What a start that failed with [e] tells. */
        private fun failure(e: Throwable, tunnel: SshTunnel, timedOut: Boolean, threads: SessionThreads): ShellEnd {
            val tunnelEnd = (tunnel.state.value as? ChannelState.Closed)?.end
            val message = e.message.orEmpty()
            return when {
                timedOut -> ShellEnd.Failed("The head unit's SSH server did not answer in time")
                e is LinkageError -> ShellEnd.Failed(missing(e))
                threads.failure != null -> ShellEnd.Failed(missing(threads.failure!!))
                e is JSchHostKeyException -> ShellEnd.HostKeyRefused
                // JSch's words when no method logged in; its exceptions for that are not public.
                e is JSchException && (message.startsWith("Auth fail") || message.startsWith("Auth cancel")) -> ShellEnd.LoginRefused
                tunnelEnd != null && tunnelEnd != ChannelEnd.ByApp -> ShellEnd.TunnelCut(tunnelEnd)
                else -> ShellEnd.Failed(message.ifEmpty { e.javaClass.simpleName })
            }
        }

        /** The words for a class or method that this phone's Android lacks. */
        private fun missing(e: Throwable) = "This phone's Android lacks a part SSH needs: ${e.javaClass.simpleName}: ${e.message.orEmpty()}"

        /** Closes the tunnel, then the session, and waits for JSch's threads to end. */
        private suspend fun teardown(tunnel: SshTunnel, session: Session?, threads: SessionThreads) {
            // The tunnel first: what JSch then writes fails at once rather than waits for credit.
            tunnel.close()
            withContext(Dispatchers.IO) {
                try {
                    session?.disconnect()
                } catch (e: Exception) {
                    // It is going all the same.
                }
                for (thread in threads.started) {
                    try {
                        thread.join(JOIN_MS)
                    } catch (e: InterruptedException) {
                        Thread.currentThread().interrupt()
                        return@withContext
                    }
                }
            }
        }
    }
}

/**
 * JSch's threads for one session, made here so that each can be waited for at the end, and so that an error a class
 * this Android lacks throws on one (which JSch does not catch) is told as the session's failure rather than end the
 * app.
 */
private class SessionThreads : ThreadFactory {
    val started = CopyOnWriteArrayList<Thread>()

    @Volatile
    var failure: LinkageError? = null

    @Volatile
    var anyEnded = false

    @Volatile
    var onEnd: () -> Unit = {}

    override fun newThread(runnable: Runnable): Thread = Thread {
        try {
            runnable.run()
        } catch (e: LinkageError) {
            failure = e
        } finally {
            anyEnded = true
            onEnd()
        }
    }.also { started += it }
}

/** Where JSch puts the shell's output, from its own thread: on to [output], which holds it back when it is not read. */
private class OutputSink : OutputStream() {
    val output = Channel<ByteArray>(OUTPUT_PIECES)

    @Volatile
    var closed = false

    @Volatile
    var onClose: () -> Unit = {}

    override fun write(byte: Int) = write(byteArrayOf(byte.toByte()), 0, 1)

    override fun write(bytes: ByteArray, offset: Int, length: Int) {
        if (length == 0) return
        // Dropped once the shell's output has been closed: the shell has ended.
        output.trySendBlocking(bytes.copyOfRange(offset, offset + length))
    }

    /** JSch's: the server's end of the shell's output, or the channel's close. */
    override fun close() {
        closed = true
        onClose()
    }

    private companion object {
        /** Pieces of output held for the reader at most, each one of the server's messages. */
        const val OUTPUT_PIECES = 64
    }
}

/** The tunnel's streams as JSch's proxy: JSch has them and no socket. */
private class TunnelProxy(private val tunnel: SshTunnel) : Proxy {
    override fun connect(socketFactory: SocketFactory?, host: String?, port: Int, timeout: Int) = Unit

    override fun getInputStream(): InputStream = tunnel.input

    override fun getOutputStream(): OutputStream = tunnel.output

    override fun getSocket(): Socket? = null

    override fun close() = tunnel.closeBlocking()
}

/** The host keys the tunnel gave, and no other: JSch refuses a server whose key is not one of them. */
private class PinnedHostKeys(lines: List<String>) : HostKeyRepository {
    private val blobs: List<ByteArray> = lines.map { requireNotNull(SshKeyLines.blob(it)) { "not a host key line: $it" } }

    override fun check(host: String?, key: ByteArray?): Int =
        if (key != null && blobs.any { it.contentEquals(key) }) HostKeyRepository.OK else HostKeyRepository.NOT_INCLUDED

    override fun add(hostkey: HostKey?, ui: UserInfo?) = Unit

    override fun remove(host: String?, type: String?) = Unit

    override fun remove(host: String?, type: String?, key: ByteArray?) = Unit

    override fun getKnownHostsRepositoryID(): String = "the head unit's, over the paired link"

    override fun getHostKey(): Array<HostKey> = emptyArray()

    /** The keys, so that JSch asks the server for their types first. */
    override fun getHostKey(host: String?, type: String?): Array<HostKey> =
        blobs.mapNotNull { blob -> runCatching { HostKey(host, blob) }.getOrNull() }.filter { type == null || it.type == type }.toTypedArray()
}

/** The key as JSch's [Identity]: its public blob and its signatures, and nothing for JSch to decode. */
private class KeyIdentity(private val key: SshKey) : Identity {
    override fun setPassphrase(passphrase: ByteArray?): Boolean = true

    override fun getPublicKeyBlob(): ByteArray = key.blob

    override fun getSignature(data: ByteArray): ByteArray = key.sshSignature(data)

    override fun getSignature(data: ByteArray, alg: String?): ByteArray = key.sshSignature(data)

    override fun getAlgName(): String = SshKeyLines.ED25519

    override fun getName(): String = SshKey.COMMENT

    override fun isEncrypted(): Boolean = false

    override fun clear() = Unit
}

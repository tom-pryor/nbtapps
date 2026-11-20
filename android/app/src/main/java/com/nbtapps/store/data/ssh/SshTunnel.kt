// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.channels.ChannelClosedException
import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelLink
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.json.JsonProfileException
import com.nbtapps.store.data.json.StrictJson
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.LinkClosedException
import com.nbtapps.store.data.link.ProtocolException
import com.nbtapps.store.data.link.RemoteException
import com.nbtapps.store.model.ChannelError
import java.io.IOException
import java.io.InputStream
import java.io.InterruptedIOException
import java.io.OutputStream
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.intOrNull

/** Why an [SshTunnel] could not be had. */
sealed interface TunnelFailure {
    /**
     * The agent refused to open the channel, with [code]: one of [ChannelError]'s ([ChannelError.OFF] for the switch,
     * [ChannelError.NOT_FOUND] where the package is not installed, [ChannelError.BUSY] during a job,
     * [ChannelError.UNAVAILABLE] when its relay did not start, [ChannelError.TOO_MANY], [ChannelError.FORBIDDEN],
     * [ChannelError.UNSUPPORTED]), or one of a later agent.
     */
    data class Refused(val code: String, val message: String) : TunnelFailure

    /** The relay answered with an error: [code] is one of [RelayError]'s, or one of a later relay. */
    data class Relay(val code: String) : TunnelFailure

    /** The relay's answer is not one it sends, as [what] says. */
    data class BadAnswer(val what: String) : TunnelFailure

    /** The channel ended before the relay's answer came, as [end] says: the link layer's reason. */
    data class Closed(val end: ChannelEnd) : TunnelFailure

    /** No answer came in time: to the channel's open (the agent's), or to the keys message (the relay's). */
    data class NoAnswer(val toKeys: Boolean) : TunnelFailure
}

/** The codes of the relay's `{"error": CODE}`. */
object RelayError {
    /** The keys message is not as the relay takes it. */
    const val BAD_KEYS = "bad-keys"

    /** The relay serves as many connections as it may. */
    const val TOO_MANY = "too-many"

    /** No host key, or the server could not be started. */
    const val FAILED = "failed"
}

/** What [SshTunnel.open] fails with. */
class SshTunnelException(val failure: TunnelFailure) : IOException(
    when (failure) {
        is TunnelFailure.Refused -> "The head unit would not open SSH: ${failure.code}"
        is TunnelFailure.Relay -> "The head unit's SSH relay refused: ${failure.code}"
        is TunnelFailure.BadAnswer -> "The head unit's SSH relay answered something else: ${failure.what}"
        is TunnelFailure.Closed -> "The SSH channel ended: ${failure.end}"
        is TunnelFailure.NoAnswer -> if (failure.toKeys) "The head unit's SSH relay did not answer in time" else "The head unit did not open SSH in time"
    },
)

/**
 * A connection to the head unit's SSH server, through the channel `ssh` of the package `org.nbtapp.ssh` and its
 * relay (`headunit/ssh/README`, "The relay"): opened with the public keys that may log in, and then a pair of byte
 * streams that an SSH client speaks over, [input] and [output], with the server's host keys to check it against
 * ([hostKeys]) and whether a shell can have a pty ([pty]).
 *
 * The streams are blocking, for a client of its own threads: what is written goes out in messages of at most the
 * channel's largest, what comes is read as bytes, their boundaries ignored; and a side that is not read holds the
 * other back through the channel's credits ([ChannelLink]). Each stream is for one thread at a time. Reading gives
 * the end (-1) once the channel has ended and all that came before is read, and [state] then says why. Closing
 * either stream closes the tunnel, as [close] does.
 */
class SshTunnel private constructor(
    private val link: ChannelLink,
    /** The server's host keys, as lines `ssh-ed25519 AAAA...`, from the relay over the paired link: the only ones to take. */
    val hostKeys: List<String>,
    /** The relay's `pty`: a pty driver runs, so a shell can have a pty; without one only commands and copies work. */
    val pty: Boolean,
) {
    /** Open, paused while a job runs on the head unit, or closed and why: the channel's. */
    val state: StateFlow<ChannelState> get() = link.state

    /** The SSH stream from the server. */
    val input: InputStream = Input()

    /** The SSH stream to the server. */
    val output: OutputStream = Output()

    /** The package's version, as the agent gave it. */
    val version: String get() = link.version

    /** Closes the tunnel: the relay then ends the server and what it ran. Closing one that has ended does nothing. */
    suspend fun close() = link.close()

    /** [close], for a thread that is not a coroutine's: a client's own. */
    fun closeBlocking() = runBlocking { link.close() }

    private inner class Input : InputStream() {
        private var piece: ByteArray? = null
        private var at = 0

        override fun read(): Int {
            val one = ByteArray(1)
            return if (read(one, 0, 1) < 0) -1 else one[0].toInt() and 0xFF
        }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            if (length == 0) return 0
            var now = piece
            if (now == null || at >= now.size) {
                now = blocking { link.receive() } ?: return -1
                piece = now
                at = 0
            }
            val n = minOf(length, now.size - at)
            now.copyInto(buffer, offset, at, at + n)
            at += n
            return n
        }

        override fun available(): Int = piece?.let { it.size - at } ?: 0

        override fun close() = closeBlocking()
    }

    private inner class Output : OutputStream() {
        override fun write(byte: Int) = write(byteArrayOf(byte.toByte()), 0, 1)

        /** The bytes go in order, cut into messages, before the next write's: one write at a time. */
        @Synchronized
        override fun write(bytes: ByteArray, offset: Int, length: Int) {
            var at = offset
            val end = offset + length
            while (at < end) {
                val n = minOf(end - at, link.maxMessage)
                val message = bytes.copyOfRange(at, at + n)
                blocking { link.send(message) }
                at += n
            }
        }

        override fun close() = closeBlocking()
    }

    companion object {
        const val PACKAGE = "org.nbtapp.ssh"
        const val CHANNEL = "ssh"

        /**
         * The longest wait for the agent's answer to the channel's open, in milliseconds. The agent answers once the
         * relay has taken the connection, and gives a relay it starts 5 s for that
         * ([com.nbtapps.store.data.link.ProtocolConstants.CHANNEL_START_WAIT_MS]), within which a relay that starts
         * first ends one left running and listens; a slow link, or a files stream taking turns with the request, adds
         * to it. Three times the agent's own wait.
         */
        const val OPEN_TIMEOUT_MS = 15_000L

        /**
         * The longest wait for the relay's answer to the keys message, in milliseconds. A relay that has just started
         * makes and reads its host key before it takes a connection the first time, with up to 20 s for each run of
         * its key tool, and may then wait up to 2 s for its pty driver; it answers `failed` itself when the key
         * cannot be had. So this is over those 42 s, which only the first connection after an install comes near;
         * later ones are answered at once.
         */
        const val ANSWER_TIMEOUT_MS = 45_000L

        /**
         * Opens the channel over [session], sends the keys message with [keys] (1 to 8 lines that pass the relay's
         * rule: [SshKeyLines]), reads the relay's answer, and returns the tunnel. A caller that is cancelled, and a
         * failure, leave nothing open.
         *
         * @throws SshTunnelException as [TunnelFailure] says.
         * @throws IllegalArgumentException for keys the relay would refuse.
         */
        suspend fun open(
            session: AgentSession,
            keys: List<String>,
            openTimeoutMs: Long = OPEN_TIMEOUT_MS,
            answerTimeoutMs: Long = ANSWER_TIMEOUT_MS,
        ): SshTunnel {
            val message = SshKeyLines.keysMessage(keys)
            val link = try {
                withTimeoutOrNull(openTimeoutMs) { session.openChannel(PACKAGE, CHANNEL) }
                    ?: throw SshTunnelException(TunnelFailure.NoAnswer(toKeys = false))
            } catch (e: RemoteException) {
                throw SshTunnelException(TunnelFailure.Refused(e.code, e.message.orEmpty()))
            } catch (e: ProtocolException) {
                throw SshTunnelException(TunnelFailure.Closed(ChannelEnd.AgentFault(e.message.orEmpty())))
            } catch (e: LinkClosedException) {
                throw SshTunnelException(TunnelFailure.Closed(ChannelEnd.LinkLost(e.message.orEmpty())))
            }
            try {
                // At most 2,352 bytes, or 3,376 with every quote and backslash of the comments escaped: one message on
                // every link that has channels.
                check(message.size <= link.maxMessage) { "the keys message is ${message.size} bytes, over the channel's ${link.maxMessage}" }
                val answer = withTimeoutOrNull(answerTimeoutMs) {
                    link.send(message)
                    link.receive()
                }
                if (answer == null) {
                    val end = (link.state.value as? ChannelState.Closed)?.end
                    throw SshTunnelException(if (end != null) TunnelFailure.Closed(end) else TunnelFailure.NoAnswer(toKeys = true))
                }
                val (hostKeys, pty) = readAnswer(answer)
                return SshTunnel(link, hostKeys, pty)
            } catch (e: ChannelClosedException) {
                withContext(NonCancellable) { link.close() }
                throw SshTunnelException(TunnelFailure.Closed(e.end))
            } catch (e: Throwable) {
                withContext(NonCancellable) { link.close() }
                throw e
            }
        }

        /**
         * The relay's answer: `{"v":1,"hostKeys":[LINE,...],"pty":BOOL}` (members a later relay adds are not looked
         * at), or `{"error":CODE}`.
         */
        internal fun readAnswer(answer: ByteArray): Pair<List<String>, Boolean> {
            val json = try {
                StrictJson.parseObject(answer)
            } catch (e: JsonProfileException) {
                throw SshTunnelException(TunnelFailure.BadAnswer("not JSON: ${e.message}"))
            }
            json["error"]?.let { error ->
                val code = (error as? JsonPrimitive)?.takeIf { it.isString }?.content
                throw SshTunnelException(if (code.isNullOrEmpty()) TunnelFailure.BadAnswer("an error with no code") else TunnelFailure.Relay(code))
            }
            if ((json["v"] as? JsonPrimitive)?.takeIf { !it.isString }?.intOrNull != 1) throw SshTunnelException(TunnelFailure.BadAnswer("v is not 1"))
            val list = json["hostKeys"] as? JsonArray ?: throw SshTunnelException(TunnelFailure.BadAnswer("no hostKeys"))
            if (list.isEmpty()) throw SshTunnelException(TunnelFailure.BadAnswer("no host key"))
            val hostKeys = list.map { item ->
                val line = (item as? JsonPrimitive)?.takeIf { it.isString }?.content
                    ?: throw SshTunnelException(TunnelFailure.BadAnswer("a host key that is not a string"))
                SshKeyLines.problem(line)?.let { throw SshTunnelException(TunnelFailure.BadAnswer("a host key that is not a key line: $it")) }
                line
            }
            val pty = (json["pty"] as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull
                ?: throw SshTunnelException(TunnelFailure.BadAnswer("no pty"))
            return hostKeys to pty
        }

        /** Runs [block] on a thread that is not a coroutine's, as a stream does: an interrupt is the stream's. */
        private fun <T> blocking(block: suspend () -> T): T = try {
            runBlocking { block() }
        } catch (e: InterruptedException) {
            Thread.currentThread().interrupt()
            throw InterruptedIOException("interrupted")
        }
    }
}

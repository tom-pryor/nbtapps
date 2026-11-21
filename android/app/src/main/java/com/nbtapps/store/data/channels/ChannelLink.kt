// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.channels

import com.nbtapps.store.data.link.AgentClient
import com.nbtapps.store.data.link.ChannelSink
import com.nbtapps.store.data.link.LinkClosedException
import com.nbtapps.store.data.link.Peer
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.link.ProtocolException
import com.nbtapps.store.model.ChannelCloseReason
import java.io.IOException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/** Why a channel ended. */
sealed interface ChannelEnd {
    /**
     * The agent closed it, for [reason]: one of [ChannelCloseReason]'s (`closed` when its program closed its end, `off`,
     * `package-changed`, `protocol` when it took this side to have broken the rules), or one of a later agent.
     */
    data class ByAgent(val reason: String) : ChannelEnd

    /** This side closed it: [ChannelLink.close]. */
    data object ByApp : ChannelEnd

    /** The agent broke the channel's rules, as [what] says. It was closed here, and the agent told `protocol`. */
    data class AgentFault(val what: String) : ChannelEnd

    /** The link ended, for [reason], which is for the user. */
    data class LinkLost(val reason: String) : ChannelEnd
}

/** How a [ChannelLink] stands. */
sealed interface ChannelState {
    data object Open : ChannelState

    /** A job runs on the head unit, which holds every channel until it has ended: nothing is sent meanwhile. */
    data object Paused : ChannelState

    data class Closed(val end: ChannelEnd) : ChannelState
}

/** What [ChannelLink.send] fails with once the channel has ended, and why it ended. */
class ChannelClosedException(val end: ChannelEnd) : IOException(
    when (end) {
        is ChannelEnd.ByAgent -> "The head unit closed the channel: ${end.reason}"
        ChannelEnd.ByApp -> "The channel was closed"
        is ChannelEnd.AgentFault -> "The head unit broke the channel's rules: ${end.what}"
        is ChannelEnd.LinkLost -> end.reason
    },
)

/**
 * One open channel to an installed package's program, over a link to the head unit's agent: what an SSH tunnel and
 * the like are built on. It is had from [com.nbtapps.store.data.link.AgentSession.openChannel], open, and ends with
 * its link at the latest.
 *
 * Messages, each of 1 to [maxMessage] bytes, go both ways in order; their boundaries are the program's to give
 * meaning to, or to ignore, as a byte stream cut into messages does. Each side sends only what the other has granted
 * (agent.hpp, "Channels"):
 *
 *  - The agent may send [ProtocolConstants.CHANNEL_CREDIT] messages at first, and as many again as are taken with
 *    [receive], granted back [GRANT_EVERY] at a time. So no more than that are ever held here, and neither side waits
 *    on the other for ever: when the agent may send nothing, more than [GRANT_EVERY] messages are held here, and taking
 *    them grants it more. A message over what was granted, an empty one, one over the size, or a DATA frame with no
 *    last flag, is the agent breaking the rules: the channel is closed with [ChannelEnd.AgentFault], the agent is told
 *    `protocol`, and the link stays up.
 *  - [send] sends what the agent granted in its answer and in each `channel.credit` since, and waits for more when it
 *    has none, and while the channel is [ChannelState.Paused]. Each message is one frame through the link's write lock,
 *    so channels and a package being sent take turns.
 *
 * A message that has come is never lost to a caller that is cancelled, nor is a grant that is due: grants go out from
 * the session's scope, and a message that has taken credit goes out whole whatever happens to its caller.
 *
 * Meant for one caller of [receive] and any number of [send]; [close] from anywhere, as often as wanted.
 *
 * Once anything here tells a caller that the channel has ended ([receive] returning null, [send] throwing
 * [ChannelClosedException], [close] returning), [state] says [ChannelState.Closed] already, with how it ended: a
 * caller may read it then to learn why, and never finds it open.
 */
class ChannelLink private constructor(
    private val peer: Peer,
    private val client: AgentClient,
    /** Where grants and the telling of the agent's faults go out from: the session's, so they end with it. */
    private val scope: CoroutineScope,
    val packageId: String,
    val channel: String,
) : ChannelSink {
    private val lock = Any()

    /** The channel's stream: set once, as the answer is read. */
    @Volatile
    private var number = 0

    // Guarded by [lock].
    private var gaveUp = false
    private var ended = false
    private val held = ArrayDeque<ByteArray>()
    /** What the agent may still send: what it was granted, less what it sent. */
    private var agentMay = ProtocolConstants.CHANNEL_CREDIT
    /** Messages taken with [receive] since the agent was last granted more. */
    private var taken = 0

    /** Counts each message that comes and the end: what a [receive] that waits waits on. */
    private val changes = MutableStateFlow(0L)

    /** What this side may still send. */
    private val credit = MutableStateFlow(0)
    private val sending = Mutex()

    private val _state = MutableStateFlow<ChannelState>(ChannelState.Open)

    /** Open, paused, or closed and why; closed before anything else tells of the end, and closed for good. */
    val state: StateFlow<ChannelState> = _state.asStateFlow()

    /** The channel's stream on its link. */
    val stream: Int get() = number

    /** The most bytes one message has, either way. */
    var maxMessage: Int = ProtocolConstants.CHANNEL_MAX_MESSAGE
        private set

    /** The version of the package, as the agent said when it opened the channel. */
    var version: String = ""
        private set

    /**
     * The next message, in the order they came; or null once the channel has ended and every message that came before
     * its end has been taken. Messages that came before [close] are dropped by it. [state] says why it ended.
     */
    suspend fun receive(): ByteArray? {
        while (true) {
            val seen = changes.value
            var grant = 0
            val message = synchronized(lock) {
                val next = held.removeFirstOrNull()
                if (next == null && ended) return null
                if (next != null) {
                    taken++
                    if (!ended && taken >= GRANT_EVERY) {
                        grant = taken
                        agentMay += taken
                        taken = 0
                    }
                }
                next
            }
            if (message != null) {
                if (grant > 0) grant(grant)
                return message
            }
            changes.first { it != seen }
        }
    }

    /**
     * Sends one message of 1 to [maxMessage] bytes: once the agent has granted credit for it, and the channel is not
     * paused. Returns once the link has taken it. Messages from several callers go in the order they got here.
     *
     * @throws ChannelClosedException once the channel has ended, also while this waits.
     * @throws IllegalArgumentException for an empty message or one over [maxMessage].
     */
    suspend fun send(bytes: ByteArray) {
        require(bytes.size in 1..maxMessage) { "a channel's message is 1 to $maxMessage bytes, not ${bytes.size}" }
        sending.withLock {
            while (true) {
                val now = _state.value
                if (now is ChannelState.Closed) throw ChannelClosedException(now.end)
                val may = credit.value
                if (now == ChannelState.Open && may > 0) {
                    if (credit.compareAndSet(may, may - 1)) break
                    continue
                }
                combine(_state, credit) { state, left -> state is ChannelState.Closed || (state == ChannelState.Open && left > 0) }.first { it }
            }
            // The credit is spent: the message goes whatever happens to the caller, or the two sides' counts part.
            withContext(NonCancellable) {
                try {
                    peer.channelMessage(number, bytes)
                } catch (e: LinkClosedException) {
                    // The link may not have told this channel yet: it ends here, so that [state] says so.
                    val lost = ChannelEnd.LinkLost(e.message.orEmpty())
                    finish(lost, discard = false)
                    throw ChannelClosedException(endOrNull() ?: lost)
                }
            }
        }
    }

    /**
     * Closes the channel: tells the agent `channel.close`, drops what came and was not taken, and ends a [send] or a
     * [receive] that waits. Closing one that has ended does nothing.
     */
    suspend fun close() {
        if (!finish(ChannelEnd.ByApp, discard = true)) return
        withContext(NonCancellable) { tell(ChannelCloseReason.CLOSED) }
    }

    // ---- what the link brings, on its reading coroutine (ChannelSink)

    override fun opened(stream: Int): Boolean = synchronized(lock) {
        if (gaveUp) return false
        number = stream
        true
    }

    override suspend fun onFrame(payload: ByteArray, last: Boolean) {
        val fault = synchronized(lock) {
            if (ended) return
            when {
                !last -> "a DATA frame with no last flag"
                payload.isEmpty() -> "an empty message"
                payload.size > ProtocolConstants.CHANNEL_MAX_MESSAGE -> "a message of ${payload.size} bytes"
                agentMay <= 0 -> "a message over the credit the phone gave"
                else -> {
                    agentMay--
                    held.addLast(payload)
                    null
                }
            }
        }
        if (fault != null) broke(fault) else changes.update { it + 1 }
    }

    override suspend fun onCredit(messages: Long) {
        if (messages !in 1..65_535) {
            broke("a channel.credit of $messages messages")
            return
        }
        credit.update { (it.toLong() + messages).coerceAtMost(MOST_CREDIT).toInt() }
    }

    override fun onClosed(reason: String) {
        finish(ChannelEnd.ByAgent(reason), discard = false)
    }

    override fun onPaused(paused: Boolean) {
        _state.update { now -> if (now is ChannelState.Closed) now else if (paused) ChannelState.Paused else ChannelState.Open }
    }

    override fun onLinkEnd(reason: String) {
        finish(ChannelEnd.LinkLost(reason), discard = false)
    }

    // ----

    /** The agent broke the rules: the channel ends here, and the agent is told `protocol`. */
    private suspend fun broke(what: String) {
        if (finish(ChannelEnd.AgentFault(what), discard = false)) tell(ChannelCloseReason.PROTOCOL)
    }

    /**
     * Ends the channel on this side, with [end]: nothing more of it is taken, and whatever waits is woken. With [discard]
     * what was held and not taken goes too. False if it had ended already.
     */
    private fun finish(end: ChannelEnd, discard: Boolean): Boolean {
        // [state] first, and the one that sets it is the one that ends the channel: whatever learns of the end, from
        // [receive], [send] or [close], finds it there already.
        while (true) {
            val now = _state.value
            if (now is ChannelState.Closed) return false
            if (_state.compareAndSet(now, ChannelState.Closed(end))) break
        }
        val stream = synchronized(lock) {
            ended = true
            if (discard) held.clear()
            number
        }
        if (stream != 0) peer.endChannel(stream, this)
        changes.update { it + 1 }
        return true
    }

    private fun endOrNull(): ChannelEnd? = (_state.value as? ChannelState.Closed)?.end

    private suspend fun tell(reason: String) {
        try {
            client.channelClose(number, reason)
        } catch (e: LinkClosedException) {
            // The channel ends with the link on the agent's side too.
        }
    }

    /** Grants the agent [messages] more, from the session's scope: a caller of [receive] that is cancelled loses none. */
    private fun grant(messages: Int) {
        scope.launch {
            try {
                client.channelCredit(number, messages)
            } catch (e: LinkClosedException) {
                // The channel ends with the link.
            }
        }
    }

    /** Whoever opened the channel gave up waiting: true if it is open already, and has to be closed. */
    private fun giveUp(): Boolean = synchronized(lock) {
        if (number != 0) return true
        gaveUp = true
        false
    }

    companion object {
        /** Messages taken before the agent is granted as many again. */
        const val GRANT_EVERY = ProtocolConstants.CHANNEL_CREDIT / 2

        /** More credit than any side can use is as good as no limit: what this side counts at most. */
        private const val MOST_CREDIT = 1L shl 20

        /**
         * Opens the channel [channel] of the installed package [packageId] over [peer], and returns it open. A caller
         * that is cancelled while it waits leaves nothing open.
         *
         * @throws com.nbtapps.store.data.link.RemoteException with the agent's refusal.
         * @throws ProtocolException for an answer this app cannot read.
         * @throws LinkClosedException if the link ends first.
         */
        internal suspend fun open(peer: Peer, client: AgentClient, scope: CoroutineScope, packageId: String, channel: String): ChannelLink {
            val link = ChannelLink(peer, client, scope, packageId, channel)
            val opened = try {
                client.channelOpen(packageId, channel, link, ProtocolConstants.CHANNEL_CREDIT)
            } catch (e: CancellationException) {
                if (link.giveUp()) link.close()
                throw e
            } catch (e: ProtocolException) {
                // The client has closed the channel the answer named, if it named one, and told the agent.
                link.finish(ChannelEnd.AgentFault(e.message.orEmpty()), discard = true)
                throw e
            }
            link.maxMessage = minOf(opened.maxMessage, ProtocolConstants.CHANNEL_MAX_MESSAGE, peer.maxFrame)
            link.version = opened.version
            // Added to: a channel.credit may have come before this coroutine saw the answer.
            link.credit.update { (it.toLong() + opened.credit).coerceAtMost(MOST_CREDIT).toInt() }
            return link
        }
    }
}

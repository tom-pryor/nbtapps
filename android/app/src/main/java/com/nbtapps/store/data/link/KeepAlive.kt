// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

/**
 * When a quiet link is pinged and when it is given up, as `spec/protocol/constants.txt` in the agent repo sets
 * it for a serial link. The agent follows the same rule from its side (`Agent::keepAlive`).
 *
 *  - With nothing heard for [pingAfterMs], a `ping` goes out, and another each time as long passes with nothing
 *    heard still.
 *  - With nothing heard for [silenceMs], the link is ended. Its pings have gone out by then, unless this side was
 *    held up: the phone slept, or its process was frozen. Then none goes out late: silent that long, the other side
 *    has given the link up by its own rule, and anything written to it now would only wait in the link.
 *  - Anything that arrives counts as heard, not only the answer to a ping.
 *
 * It holds no clock and sends nothing: [step] is told the time and says what is due. The time has to go on while
 * the phone sleeps, or a long sleep would look short.
 */
class KeepAlive(
    private val pingAfterMs: Long,
    private val silenceMs: Long,
    /** When the link began to be watched. */
    startMs: Long,
) {
    sealed interface Step {
        /** Nothing is due for [millis]. */
        data class Wait(val millis: Long) : Step

        /** Send a `ping` now. */
        data object Ping : Step

        /**
         * End the link: nothing has been heard for [silentMs], pings included. [pinged]: the pings went out, and none
         * was answered; otherwise this side was held up for longer than the silence allowed, and sent nothing.
         */
        data class Close(val silentMs: Long, val pinged: Boolean = true) : Step
    }

    private var heardMs = startMs
    private var pingedMs = startMs

    /** The pings sent since something was last heard. */
    private var pings = 0

    /** What is due at [nowMs], on a link where bytes last arrived at [lastHeardMs]. A [Step.Ping] counts as sent. */
    fun step(nowMs: Long, lastHeardMs: Long): Step {
        if (lastHeardMs > heardMs) {
            heardMs = lastHeardMs
            pings = 0
        }
        val silent = nowMs - heardMs
        if (silent >= silenceMs) return Step.Close(silent, pinged = pings >= silenceMs / pingAfterMs - 1)
        val wait = maxOf(pingAfterMs - silent, pingAfterMs - (nowMs - pingedMs))
        if (wait > 0) return Step.Wait(minOf(wait, silenceMs - silent))
        pingedMs = nowMs
        pings++
        return Step.Ping
    }
}

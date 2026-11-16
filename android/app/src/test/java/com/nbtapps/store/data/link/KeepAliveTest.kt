// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.link.KeepAlive.Step
import org.junit.Assert.assertEquals
import org.junit.Test

/** The keep-alive rule by itself, with the times a serial link has: a ping after 10 s, the end after 30 s. */
class KeepAliveTest {

    private val start = 1_000L
    private fun rule() = KeepAlive(pingAfterMs = 10_000, silenceMs = 30_000, startMs = start)

    @Test
    fun `nothing is due until the link has been quiet for the first time`() {
        val rule = rule()
        assertEquals(Step.Wait(10_000), rule.step(start, start))
        assertEquals(Step.Wait(1), rule.step(start + 9_999, start))
        assertEquals(Step.Ping, rule.step(start + 10_000, start))
    }

    @Test
    fun `a silent link is pinged twice and then ended`() {
        val rule = rule()
        assertEquals(Step.Ping, rule.step(start + 10_000, start))
        assertEquals(Step.Wait(10_000), rule.step(start + 10_000, start))
        assertEquals(Step.Wait(1), rule.step(start + 19_999, start))
        assertEquals(Step.Ping, rule.step(start + 20_000, start))
        assertEquals(Step.Wait(10_000), rule.step(start + 20_000, start))
        assertEquals(Step.Close(30_000), rule.step(start + 30_000, start))
    }

    @Test
    fun `a link that answers its pings is pinged again as long after, and never ended`() {
        val rule = rule()
        var heard = start
        var now = start
        repeat(20) {
            now += 10_000
            assertEquals(Step.Ping, rule.step(now, heard))
            // The answer takes 40 ms, and the quiet time is counted from it.
            heard = now + 40
            now = heard
            assertEquals(Step.Wait(10_000), rule.step(now, heard))
        }
    }

    @Test
    fun `anything heard counts, not only an answer`() {
        val rule = rule()
        // An event came 6 s in, so 10 s in the link has been quiet for 4 s only.
        assertEquals(Step.Wait(6_000), rule.step(start + 10_000, start + 6_000))
        assertEquals(Step.Ping, rule.step(start + 16_000, start + 6_000))
        // Heard again after that ping and one more: the count of unanswered pings starts over.
        assertEquals(Step.Ping, rule.step(start + 26_000, start + 6_000))
        assertEquals(Step.Wait(10_000), rule.step(start + 35_000, start + 35_000))
        assertEquals(Step.Ping, rule.step(start + 45_000, start + 35_000))
        assertEquals(Step.Ping, rule.step(start + 55_000, start + 35_000))
        assertEquals(Step.Close(30_000), rule.step(start + 65_000, start + 35_000))
    }

    @Test
    fun `a link silent for longer than allowed is ended at once, with no late ping first`() {
        val rule = rule()
        // This side was held up for 45 s, asleep or frozen, and sent nothing meanwhile: the other side has given the
        // link up by now, and a ping would only wait in the link.
        assertEquals(Step.Close(45_000, pinged = false), rule.step(start + 45_000, start))
    }

    @Test
    fun `pings from before something was heard do not count against the silence after it`() {
        val rule = rule()
        assertEquals(Step.Ping, rule.step(start + 10_000, start))
        assertEquals(Step.Ping, rule.step(start + 20_000, start))
        // Heard at 25 s: the count starts over, and so does the wait for the first ping.
        assertEquals(Step.Wait(5_000), rule.step(start + 30_000, start + 25_000))
        assertEquals(Step.Ping, rule.step(start + 35_000, start + 25_000))
        // Then this side was held up until 60 s: 35 s of silence, and only one of its pings went out.
        assertEquals(Step.Close(35_000, pinged = false), rule.step(start + 60_000, start + 25_000))
    }

    @Test
    fun `the wait never runs past the moment the silence is up`() {
        // A silence that is not a whole number of ping times: after the second ping the wait is the 5 s that are left.
        val quiet = KeepAlive(pingAfterMs = 10_000, silenceMs = 25_000, startMs = 0)
        assertEquals(Step.Ping, quiet.step(10_000, 0))
        assertEquals(Step.Ping, quiet.step(20_000, 0))
        assertEquals(Step.Wait(5_000), quiet.step(20_000, 0))
        assertEquals(Step.Close(25_000), quiet.step(25_000, 0))
    }

    @Test
    fun `the number of pings follows from the two times`() {
        // Silence allowed for twice the ping time: one ping, then the end.
        val rule = KeepAlive(pingAfterMs = 200, silenceMs = 400, startMs = 0)
        assertEquals(Step.Ping, rule.step(200, 0))
        assertEquals(Step.Close(400), rule.step(400, 0))
    }
}

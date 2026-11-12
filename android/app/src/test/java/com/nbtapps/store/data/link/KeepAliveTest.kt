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
    fun `silence that no ping has tested does not end the link`() {
        val rule = rule()
        // This side was held up for 45 s and sent nothing meanwhile: the other side gets its two pings first.
        assertEquals(Step.Ping, rule.step(start + 45_000, start))
        assertEquals(Step.Wait(10_000), rule.step(start + 45_000, start))
        assertEquals(Step.Ping, rule.step(start + 55_000, start))
        assertEquals(Step.Close(65_000), rule.step(start + 65_000, start))
    }

    @Test
    fun `pings from before something was heard do not count against the silence after it`() {
        val rule = rule()
        assertEquals(Step.Ping, rule.step(start + 10_000, start))
        assertEquals(Step.Ping, rule.step(start + 20_000, start))
        // Heard at 25 s, then this side was held up until 60 s: 35 s of silence, and no ping has tested it yet.
        assertEquals(Step.Ping, rule.step(start + 60_000, start + 25_000))
        assertEquals(Step.Ping, rule.step(start + 70_000, start + 25_000))
        assertEquals(Step.Close(55_000), rule.step(start + 80_000, start + 25_000))
    }

    @Test
    fun `the number of pings follows from the two times`() {
        // Silence allowed for twice the ping time: one ping, then the end.
        val rule = KeepAlive(pingAfterMs = 200, silenceMs = 400, startMs = 0)
        assertEquals(Step.Ping, rule.step(200, 0))
        assertEquals(Step.Close(400), rule.step(400, 0))
    }
}

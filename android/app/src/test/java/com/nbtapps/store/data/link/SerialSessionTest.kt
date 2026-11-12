// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.crypto.sha256Hex
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.LinkClosing
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.JsonObject
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What a Bluetooth serial link gets from the app: the smaller frame in `hello`, and the keep-alive. The agent's
 * end of the link is driven by hand. The keep-alive runs on the real clock with times cut to fractions of a
 * second; what is asserted of time is only that something did not come too soon, which a slow machine cannot
 * make false.
 */
class SerialSessionTest {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val sessions = ArrayList<AgentSession>()

    private val phoneId = "5f2c9a1e07b34d6688a0c1d2e3f40516"

    @After
    fun tearDown() {
        sessions.forEach { it.close("the test is over") }
        scope.cancel()
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTimeout(60_000) { block() } }

    /** A session and the agent's end of its link. Left alone, the keep-alive times are far beyond a test's length. */
    private fun session(
        kind: String = Link.SERIAL,
        pingAfterMs: Long = 600_000,
        silenceMs: Long = 1_800_000,
        capacity: Int = Int.MAX_VALUE,
    ): Pair<AgentSession, AgentEnd> {
        val (phone, other) = MemoryLink.pair(capacity, kind)
        return AgentSession(phone, scope, pingAfterMs = pingAfterMs, silenceMs = silenceMs).also { sessions += it } to AgentEnd(other)
    }

    private suspend fun CoroutineScope.greet(session: AgentSession, agent: AgentEnd, agentFrame: Int = 4096): JsonObject {
        val hello = async { session.client.hello(phoneId, "Pixel 9") }
        val sent = agent.answerHello(maxFrame = agentFrame)
        hello.await()
        return sent
    }

    private fun millis() = System.nanoTime() / 1_000_000

    /** Reads the phone's next message, which must be a `ping` with no parameters, and returns its id. */
    private fun AgentEnd.expectPing(): String {
        val message = message()
        assertEquals("\"ping\"", message["method"].toString())
        assertEquals("{}", message["params"].toString())
        return message["id"].toString()
    }

    @Test
    fun `hello offers 4,096 bytes on a serial link, and the lower of the two values holds`() = test {
        for ((agentFrame, agreed) in listOf(4096 to 4096, 65536 to 4096, 1024 to 1024)) {
            val (session, agent) = session()
            assertEquals("4096", greet(session, agent, agentFrame)["maxFrame"].toString())
            assertEquals(agreed, session.peer.maxFrame)
        }
    }

    @Test
    fun `hello offers 65,536 bytes on any other link`() = test {
        for (kind in listOf("tcp", "local", "memory")) {
            val (session, agent) = session(kind)
            assertEquals(kind, "65536", greet(session, agent, agentFrame = 65536)["maxFrame"].toString())
            assertEquals(kind, 65536, session.peer.maxFrame)
        }
    }

    @Test
    fun `a blob goes out in frames of the size the serial link agreed`() = test {
        val (session, agent) = session()
        greet(session, agent, agentFrame = 65536)
        val bytes = ByteArray(10_000) { (it * 31).toByte() }
        val hash = sha256Hex(bytes)
        session.blobs.offer(mapOf(hash to Blob.OfBytes(bytes)))

        agent.send("""{"id":1,"method":"blob.fetch","params":{"sha256":"$hash","offset":0}}""")
        assertEquals("""{"stream":1,"size":10000}""", agent.message()["result"].toString())
        val frames = ArrayList<Frame>()
        do frames += agent.frame() while (!frames.last().last)
        assertEquals(listOf(4096, 4096, 1808), frames.map { it.payload.size })
        assertArrayEquals(bytes, frames.flatMap { it.payload.asList() }.toByteArray())
    }

    @Test
    fun `the agent's ping is answered with an empty result, on any link`() = test {
        for (kind in listOf(Link.SERIAL, "tcp")) {
            val (session, agent) = session(kind)
            greet(session, agent)
            agent.send("""{"id":7,"method":"ping","params":{}}""")
            assertEquals(kind, """{"id":7,"result":{}}""", agent.message().toString())
        }
    }

    @Test
    fun `a quiet serial link is pinged, and one that answers stays open`() = test {
        val (session, agent) = session(pingAfterMs = 150, silenceMs = 450)
        val greeted = millis()
        greet(session, agent)
        // Five pings answered take longer than the silence allowed.
        repeat(5) { round ->
            val id = agent.expectPing()
            if (round == 0) assertTrue("the first ping came before the link had been quiet for 150 ms", millis() - greeted >= 150)
            agent.send("""{"id":$id,"result":{}}""")
        }
        assertTrue(millis() - greeted >= 5 * 150)
        assertTrue(session.peer.isOpen)
        // A ping is a request like any other: what else is asked meanwhile still finds its answer.
        val info = async { session.peer.request("info.get") }
        var message = agent.message()
        while (message["method"].toString() == "\"ping\"") message = agent.message()
        agent.send("""{"id":${message["id"]},"result":{"agentVersion":"1.0.0"}}""")
        assertEquals("""{"agentVersion":"1.0.0"}""", info.await().toString())
    }

    @Test
    fun `an agent that answers ping with an error has still answered`() = test {
        val (session, agent) = session(pingAfterMs = 150, silenceMs = 450)
        greet(session, agent)
        repeat(5) { agent.send("""{"id":${agent.expectPing()},"error":{"code":"unsupported","message":"no ping here"}}""") }
        assertTrue(session.peer.isOpen)
    }

    @Test
    fun `a serial link that stays silent gets its two pings and is then ended, with a reason to show`() = test {
        val (session, agent) = session(pingAfterMs = 150, silenceMs = 450)
        val greeted = millis()
        greet(session, agent)
        agent.send("""{"id":${agent.expectPing()},"result":{}}""")
        val waiting = async {
            try {
                session.peer.request("info.get")
                "answered"
            } catch (e: LinkClosedException) {
                e.message
            }
        }
        // From here the agent says nothing more. It sees the request and two pings, in whichever order, and then
        // the end. (A third ping is there only if this test was held up before its one answer: KeepAliveTest has
        // the exact count.)
        var frames = 0
        while (!agent.closedByPhone()) frames++
        assertTrue("$frames frames before the end", frames >= 3)
        assertTrue("the link was ended before it had been silent for 450 ms", millis() - greeted >= 150 + 450)
        assertEquals("The head unit stopped answering", session.ended.await())
        assertEquals("The head unit stopped answering", waiting.await())
        assertFalse(session.peer.isOpen)
    }

    @Test
    fun `link-closing ends the link from the phone's side at once, and nothing is sent after it`() = test {
        for (kind in listOf(Link.SERIAL, "tcp")) {
            // The keep-alive is far off, so whatever ends this link is not it.
            val (session, agent) = session(kind)
            greet(session, agent)
            // A request is waiting for its answer when the agent says that it goes.
            val waiting = async {
                try {
                    session.peer.request("info.get")
                    "answered"
                } catch (e: LinkClosedException) {
                    e.message
                }
            }
            assertEquals(kind, "\"info.get\"", agent.message()["method"].toString())

            agent.send("""{"event":"link.closing","params":{"reason":"stopping"}}""")

            // The agent's end stays open, as the real agent's does: it is the phone that ends the link. The next
            // thing the agent reads is that end, with no frame before it.
            assertTrue("$kind: the phone sent a frame after link.closing", agent.closedByPhone())
            assertEquals(kind, AgentText.closing(LinkClosing.STOPPING), session.ended.await())
            assertEquals(kind, LinkClosing.STOPPING, session.closing)
            assertEquals(kind, AgentText.closing(LinkClosing.STOPPING), waiting.await())
            assertFalse(session.peer.isOpen)
        }
    }

    @Test
    fun `link-closing with a reason the app does not know, or with none, ends the link all the same`() = test {
        for ((params, reason) in listOf("""{"reason":"moving-house"}""" to "moving-house", "{}" to "", """{"reason":7}""" to "")) {
            val (session, agent) = session()
            greet(session, agent)

            agent.send("""{"event":"link.closing","params":$params}""")

            assertTrue(params, agent.closedByPhone())
            assertEquals(params, "The head unit's agent closed the link", session.ended.await())
            assertEquals(params, reason, session.closing)
        }
    }

    @Test
    fun `a link that ends without link-closing has no reason of the agent's`() = test {
        val (session, agent) = session()
        greet(session, agent)
        agent.close()
        assertEquals("The head unit closed the link", session.ended.await())
        assertEquals(null, session.closing)
    }

    @Test
    fun `anything the agent sends counts as heard`() = test {
        val (session, agent) = session(pingAfterMs = 500, silenceMs = 1_500)
        greet(session, agent)
        // The agent talks for a second, in events the app does not even know: no ping is due meanwhile.
        var last = 0L
        repeat(50) {
            delay(20)
            last = millis()
            agent.send("""{"event":"weather.changed","params":{"sky":"grey"}}""")
        }
        agent.expectPing()
        // Had the events not counted, the ping would have been sent half a second into them, and be waiting here already.
        assertTrue("a ping was sent while the agent was talking", millis() - last >= 500)
    }

    @Test
    fun `nothing is pinged before hello, and no other kind of link is pinged at all`() = test {
        // Serial, not yet greeted: the first thing the agent ever reads is the hello.
        val (serial, serialAgent) = session(pingAfterMs = 50, silenceMs = 150)
        delay(400)
        assertTrue(serial.peer.isOpen)
        assertNotNull(greet(serial, serialAgent)["phoneId"])

        // Not serial: long after the times have passed, the next thing the agent reads is what the app asked.
        val (tcp, tcpAgent) = session("tcp", pingAfterMs = 50, silenceMs = 150)
        greet(tcp, tcpAgent)
        delay(400)
        assertTrue(tcp.peer.isOpen)
        val info = async { tcp.peer.request("info.get") }
        val asked = tcpAgent.message()
        assertEquals("\"info.get\"", asked["method"].toString())
        tcpAgent.send("""{"id":${asked["id"]},"result":{}}""")
        info.await()
    }

    @Test
    fun `a link whose writes are stuck is ended all the same`() = test {
        // Each direction holds one write. The agent reads the hello and then nothing more, so the first ping fills
        // the link and the second waits in its write, as on a serial link that has stopped moving.
        val (session, agent) = session(pingAfterMs = 150, silenceMs = 450, capacity = 1)
        greet(session, agent)
        assertEquals("The head unit stopped answering", withTimeout(10_000) { session.ended.await() })
    }
}

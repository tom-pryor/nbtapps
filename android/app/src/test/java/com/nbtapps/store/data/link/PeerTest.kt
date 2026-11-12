// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** The message layer's rules, with the agent's end of the link driven by hand. */
class PeerTest {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val ends = MemoryLink.pair()
    private val session = AgentSession(ends.first, scope)
    private val agent = AgentEnd(ends.second)

    @After
    fun tearDown() {
        session.close("the test is over")
        scope.cancel()
    }

    // On the shared pool, not the test's own thread: the agent's end is read with blocking calls, and what the
    // phone's side has started must run meanwhile.
    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTimeout(30_000) { block() } }

    private suspend fun closedReason(block: suspend () -> Unit): String = try {
        block()
        fail("the request was answered")
        ""
    } catch (e: LinkClosedException) {
        e.message.orEmpty()
    }

    @Test
    fun `the magic bytes go first, requests count from one, and each answer finds its request`() = test {
        val first = async { session.peer.request("info.get") }
        agent.expectMagic()
        val one = agent.message()
        assertEquals("1", one["id"].toString())
        assertEquals("\"info.get\"", one["method"].toString())
        assertEquals("{}", one["params"].toString())

        val second = async { session.peer.request("packages.list", buildJsonObject { put("after", "org.example.a") }) }
        val two = agent.message()
        assertEquals("2", two["id"].toString())
        assertEquals("""{"after":"org.example.a"}""", two["params"].toString())

        // Answered out of order, and with a field the app does not know.
        agent.send("""{"id":2,"result":{"packages":[],"more":false},"extra":true}""")
        agent.send("""{"id":1,"result":{"agentVersion":"1.0.0"}}""")
        assertEquals("""{"packages":[],"more":false}""", second.await().toString())
        assertEquals("""{"agentVersion":"1.0.0"}""", first.await().toString())
    }

    @Test
    fun `an error response comes out with its code`() = test {
        val request = async {
            try {
                session.peer.request("job.start")
                "no error"
            } catch (e: RemoteException) {
                "${e.code}: ${e.message}"
            }
        }
        agent.expectMagic()
        agent.message()
        agent.send("""{"id":1,"error":{"code":"plan-stale","message":"the head unit changed"}}""")
        assertEquals("plan-stale: the head unit changed", request.await())
    }

    @Test
    fun `an answer to nothing that was asked is ignored, and so is an unknown event`() = test {
        val request = async { session.peer.request("info.get") }
        agent.expectMagic()
        agent.message()
        agent.send("""{"id":99,"result":{}}""")
        agent.send("""{"event":"weather.changed","params":{"sky":"grey"}}""")
        agent.send("""{"id":1,"result":{"ok":true}}""")
        assertEquals("""{"ok":true}""", request.await().toString())
        assertTrue(session.peer.isOpen)
    }

    @Test
    fun `a message outside the JSON profile closes the link, and what was waiting fails`() = test {
        val request = async { closedReason { session.peer.request("info.get") } }
        agent.expectMagic()
        agent.message()
        agent.send("""{"id":1,"id":1,"result":{}}""")
        assertTrue(request.await().isNotEmpty())
        assertTrue(agent.closedByPhone())
        assertFalse(session.peer.isOpen)
        // And nothing more can be asked.
        assertTrue(closedReason { session.peer.request("info.get") }.isNotEmpty())
    }

    @Test
    fun `a message that is no request, response or event closes the link`() = test {
        for (bad in listOf("""{"hello":"there"}""", """{"id":1,"result":{},"error":{"code":"x"}}""", """{"id":"1","method":"blob.fetch"}""")) {
            val (phone, other) = MemoryLink.pair()
            val one = AgentSession(phone, scope)
            val end = AgentEnd(other)
            val request = async { closedReason { one.peer.request("info.get") } }
            end.expectMagic()
            end.message()
            end.send(bad)
            assertTrue(bad, request.await().isNotEmpty())
            assertTrue(bad, end.closedByPhone())
        }
    }

    @Test
    fun `a DATA frame from the agent closes the link`() = test {
        val request = async { closedReason { session.peer.request("info.get") } }
        agent.expectMagic()
        agent.message()
        agent.raw(FrameCodec.encode(FrameCodec.TYPE_DATA, FrameCodec.FLAG_LAST, 1, byteArrayOf(1, 2, 3)))
        assertTrue(request.await().isNotEmpty())
        assertTrue(agent.closedByPhone())
    }

    @Test
    fun `a broken frame closes the link`() = test {
        val request = async { closedReason { session.peer.request("info.get") } }
        agent.expectMagic()
        agent.message()
        val frame = FrameCodec.encode(FrameCodec.TYPE_MSG, 0, 0, """{"id":1,"result":{}}""".encodeToByteArray())
        frame[frame.size - 1] = (frame[frame.size - 1] + 1).toByte()
        agent.raw(frame)
        assertTrue(request.await().isNotEmpty())
        assertTrue(agent.closedByPhone())
    }

    @Test
    fun `the agent closing the link fails what was waiting, with a reason to show`() = test {
        val request = async { closedReason { session.peer.request("job.plan") } }
        agent.expectMagic()
        agent.message()
        agent.close()
        assertTrue(request.await().isNotEmpty())
        assertEquals(session.peer.closeReason, session.ended.await())
    }

    @Test
    fun `hello agrees the frame size, and neither side goes above it afterwards`() = test {
        val hello = async { session.client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9") }
        agent.answerHello(maxFrame = 1024)
        assertEquals(1024, hello.await().maxFrame)
        assertEquals(1024, session.peer.maxFrame)

        // A request that would not fit is refused here, before it reaches the link.
        val big = buildJsonObject { put("text", "x".repeat(2000)) }
        assertTrue(closedReason { session.peer.request("job.plan", big) }.contains("fit"))
        assertTrue(session.peer.isOpen)

        // A frame above the agreed size from the agent is a framing error.
        val waiting = async { closedReason { session.peer.request("info.get") } }
        agent.message()
        agent.send("""{"id":2,"result":{"text":"${"y".repeat(2000)}"}}""")
        assertTrue(waiting.await().isNotEmpty())
        assertTrue(agent.closedByPhone())
    }

    @Test
    fun `an agent that is no agent, or shares no version, is refused at hello`() = test {
        val answers = listOf(
            """{"protocolMin":1,"protocolMax":1,"role":"phone","headUnitId":"000102030405060708090a0b0c0d0e0f","agentVersion":"1.0.0","maxFrame":65536}""",
            """{"protocolMin":2,"protocolMax":3,"role":"agent","headUnitId":"000102030405060708090a0b0c0d0e0f","agentVersion":"1.0.0","maxFrame":65536}""",
            """{"protocolMin":1,"protocolMax":1,"role":"agent","headUnitId":"not an id","agentVersion":"1.0.0","maxFrame":65536}""",
            """{"protocolMin":1,"protocolMax":1,"role":"agent","headUnitId":"000102030405060708090a0b0c0d0e0f","agentVersion":"1.0.0","maxFrame":512}""",
            """{"protocolMin":1,"protocolMax":1,"role":"agent"}""",
        )
        for (answer in answers) {
            val (phone, other) = MemoryLink.pair()
            val one = AgentSession(phone, scope)
            val end = AgentEnd(other)
            val hello = async {
                try {
                    one.client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9")
                    null
                } catch (e: ProtocolException) {
                    e.message
                }
            }
            end.expectMagic()
            end.send("""{"id":${end.message()["id"]},"result":$answer}""")
            assertTrue(answer, hello.await() != null)
            // Closed by the refusal itself, at once: not by a link that merely went quiet afterwards.
            assertTrue("the link stayed open after $answer", withTimeoutOrNull(5_000) { one.ended.await() } != null)
            assertTrue(answer, end.closedByPhone())
        }
    }

    @Test
    fun `a request the phone has no answer for gets unsupported, and bad params get bad-request`() = test {
        val hello = async { session.client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9") }
        agent.answerHello()
        hello.await()

        agent.send("""{"id":1,"method":"job.plan","params":{}}""")
        assertEquals("\"unsupported\"", (agent.message()["error"] as JsonObject)["code"].toString())
        agent.send("""{"id":2,"method":"blob.fetch","params":[1,2]}""")
        assertEquals("\"bad-request\"", (agent.message()["error"] as JsonObject)["code"].toString())
        agent.send("""{"id":3,"method":"blob.fetch","params":{"sha256":"${"0".repeat(64)}","offset":0}}""")
        val reply = agent.message()
        assertEquals("3", reply["id"].toString())
        assertEquals("\"not-found\"", (reply["error"] as JsonObject)["code"].toString())
        assertNull(reply["result"])
        assertTrue(session.peer.isOpen)
    }
}

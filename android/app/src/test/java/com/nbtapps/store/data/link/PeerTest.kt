// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import java.io.ByteArrayOutputStream
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CompletableDeferred
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

    /** What a stream brought: its frames' bytes, whether its last came, and how it ended without one. */
    private class Recorded : StreamSink {
        val bytes = ByteArrayOutputStream()
        val frames = AtomicInteger()
        val ended = CompletableDeferred<String>()

        override fun onData(bytes: ByteArray, last: Boolean) {
            synchronized(this) { this.bytes.write(bytes) }
            frames.incrementAndGet()
            if (last) ended.complete("last")
        }

        override fun onEnd(code: String?, message: String) {
            ended.complete("${code ?: "link"}: $message")
        }
    }

    private fun data(stream: Int, bytes: String, last: Boolean = false) =
        FrameCodec.encode(FrameCodec.TYPE_DATA, if (last) FrameCodec.FLAG_LAST else 0, stream, bytes.encodeToByteArray())

    private fun msg(json: String) = FrameCodec.encode(FrameCodec.TYPE_MSG, 0, 0, json.encodeToByteArray())

    @Test
    fun `a stream the phone asked for takes the DATA that comes with its answer, and ends with its last frame`() = test {
        val sink = Recorded()
        val answer = async { session.peer.requestStream("files.fetch", buildJsonObject { put("path", "a.log") }, sink) }
        agent.expectMagic()
        agent.message()
        // The answer and the stream's frames in one write, as they may come off the link in one read.
        agent.raw(msg("""{"id":1,"result":{"stream":7,"size":11,"compress":"none"}}""") + data(7, "hello ") + data(7, "world", last = true))
        assertEquals("""{"stream":7,"size":11,"compress":"none"}""", answer.await().toString())
        assertEquals("last", sink.ended.await())
        assertEquals("hello world", sink.bytes.toString())
        assertEquals(2, sink.frames.get())
        assertTrue(session.peer.isOpen)

        // The stream is over: one more frame of it closes the link.
        agent.raw(data(7, "late"))
        assertTrue(agent.closedByPhone())
        assertEquals(2, sink.frames.get())
    }

    @Test
    fun `an answer that names no stream opens none, and the stream number stays free`() = test {
        val sink = Recorded()
        val answer = async { session.peer.requestStream("files.fetch", JsonObject(emptyMap()), sink) }
        agent.expectMagic()
        agent.message()
        agent.send("""{"id":1,"result":{"size":0,"compress":"deflate"}}""")
        answer.await()
        agent.raw(data(1, "nothing was asked"))
        assertTrue(agent.closedByPhone())
        assertEquals(0, sink.frames.get())
        assertFalse(sink.ended.isCompleted)
    }

    @Test
    fun `files-error ends the stream it names, with its code, and the link stays`() = test {
        val sink = Recorded()
        val answer = async { session.peer.requestStream("files.fetch", JsonObject(emptyMap()), sink) }
        agent.expectMagic()
        agent.message()
        agent.raw(msg("""{"id":1,"result":{"stream":3,"size":100,"compress":"none"}}""") + data(3, "part"))
        answer.await()
        agent.send("""{"event":"files.error","params":{"stream":3,"code":"io-error","message":"read failed"}}""")
        assertEquals("io-error: read failed", sink.ended.await())
        assertEquals("part", sink.bytes.toString())
        // Asked again, the stream number may come again.
        val again = Recorded()
        val second = async { session.peer.requestStream("files.fetch", JsonObject(emptyMap()), again) }
        agent.message()
        agent.raw(msg("""{"id":2,"result":{"stream":3,"size":2,"compress":"none"}}""") + data(3, "ok", last = true))
        second.await()
        assertEquals("last", again.ended.await())
        assertTrue(session.peer.isOpen)
    }

    @Test
    fun `a stream that the phone ends takes no more, and the link ending ends one that is open`() = test {
        val first = Recorded()
        val answer = async { session.peer.requestStream("files.fetch", JsonObject(emptyMap()), first) }
        agent.expectMagic()
        agent.message()
        agent.raw(msg("""{"id":1,"result":{"stream":5,"size":100,"compress":"none"}}""") + data(5, "a"))
        answer.await()
        val cancel = async { session.client.filesCancel(5) }
        assertEquals("\"files.cancel\"", agent.message()["method"].toString())
        agent.raw(data(5, "queued before the cancel") + msg("""{"id":2,"result":{}}"""))
        cancel.await()
        assertEquals("aqueued before the cancel", first.bytes.toString())
        assertFalse("ended by the phone, it is not told", first.ended.isCompleted)

        val second = Recorded()
        val open = async { session.peer.requestStream("files.fetch", JsonObject(emptyMap()), second) }
        agent.message()
        agent.send("""{"id":3,"result":{"stream":6,"size":100,"compress":"none"}}""")
        open.await()
        agent.close()
        assertTrue(second.ended.await().startsWith("link: "))
    }

    @Test
    fun `an answer that names a stream that is open already closes the link`() = test {
        val answer = async { session.peer.requestStream("files.fetch", JsonObject(emptyMap()), Recorded()) }
        agent.expectMagic()
        agent.message()
        agent.send("""{"id":1,"result":{"stream":4,"size":100,"compress":"none"}}""")
        answer.await()
        val twice = async { closedReason { session.peer.requestStream("files.fetch", JsonObject(emptyMap()), Recorded()) } }
        agent.message()
        agent.send("""{"id":2,"result":{"stream":4,"size":100,"compress":"none"}}""")
        assertTrue(twice.await().isNotEmpty())
        assertTrue(agent.closedByPhone())
    }

    @Test
    fun `a DATA frame of a stream the phone did not ask for closes the link`() = test {
        val request = async { closedReason { session.peer.request("info.get") } }
        agent.expectMagic()
        agent.message()
        agent.raw(FrameCodec.encode(FrameCodec.TYPE_DATA, FrameCodec.FLAG_LAST, 1, byteArrayOf(1, 2, 3)))
        assertTrue(request.await().isNotEmpty())
        assertTrue(agent.closedByPhone())
    }

    /** What a channel's sink was given, in order: "data TEXT[ last]", "credit N", "closed REASON", "paused" and so on. */
    private class RecordedChannel(private val wanted: Boolean = true) : ChannelSink {
        val heard = LinkedBlockingQueue<String>()

        @Volatile
        var stream = 0

        override fun opened(stream: Int): Boolean {
            this.stream = stream
            heard.put("opened $stream")
            return wanted
        }

        override suspend fun onFrame(payload: ByteArray, last: Boolean) = heard.put("data ${payload.decodeToString()}${if (last) " last" else ""}")

        override suspend fun onCredit(messages: Long) = heard.put("credit $messages")

        override fun onClosed(reason: String) = heard.put("closed $reason")

        override fun onPaused(paused: Boolean) = heard.put(if (paused) "paused" else "resumed")

        override fun onLinkEnd(reason: String) = heard.put("link ended")

        fun next(): String = heard.poll(10, TimeUnit.SECONDS) ?: "nothing"
    }

    private fun openParams() = buildJsonObject {
        put("package", "org.example.a")
        put("channel", "echo")
    }

    private val answer32768 = """{"id":1,"result":{"stream":32768,"maxMessage":4096,"credit":16,"version":"1.0.0"}}"""

    /** Whether the link still serves the agent: a ping from it is answered. */
    private fun AgentEnd.pingAnswered(id: Int = 900): Boolean {
        send("""{"id":$id,"method":"ping","params":{}}""")
        return message().toString() == """{"id":$id,"result":{}}"""
    }

    @Test
    fun `a channel's frames and events go to its sink from the moment its answer is read, in their order`() = test {
        val sink = RecordedChannel()
        val open = async { session.peer.requestChannel(openParams(), sink) }
        agent.expectMagic()
        assertEquals("\"channel.open\"", agent.message()["method"].toString())
        // The answer, two messages, a grant, a pause and the agent's close, all in one write: one read of the link.
        agent.raw(
            msg(answer32768) + data(32768, "one", last = true) + msg("""{"event":"channel.credit","params":{"stream":32768,"messages":3}}""") +
                data(32768, "two", last = true) + msg("""{"event":"channel.paused","params":{}}""") +
                msg("""{"event":"channel.close","params":{"stream":32768,"reason":"off"}}"""),
        )
        assertEquals("32768", open.await()["stream"].toString())
        assertEquals(
            listOf("opened 32768", "data one last", "credit 3", "data two last", "paused", "closed off"),
            List(6) { sink.next() },
        )
        assertTrue(session.peer.channel(32768) === sink)
        assertEquals(listOf(sink), session.peer.openChannels())
        assertTrue(agent.pingAnswered())
    }

    @Test
    fun `a DATA frame of a channel that is not open is dropped, and the link stays`() = test {
        val sink = RecordedChannel()
        val open = async { session.peer.requestChannel(openParams(), sink) }
        agent.expectMagic()
        agent.message()
        agent.send(answer32768)
        open.await()
        assertEquals("opened 32768", sink.next())
        assertTrue(session.peer.endChannel(32768, sink))
        assertFalse("ended once", session.peer.endChannel(32768, sink))

        // One that crossed the close, one of a number never opened, and an event of each: all dropped.
        agent.raw(
            data(32768, "late", last = true) + data(40000, "never", last = true) +
                msg("""{"event":"channel.credit","params":{"stream":32768,"messages":1}}""") +
                msg("""{"event":"channel.close","params":{"stream":40000,"reason":"closed"}}"""),
        )
        assertTrue(agent.pingAnswered())
        assertTrue(session.peer.isOpen)
        assertNull(sink.heard.poll())
    }

    @Test
    fun `a DATA frame of an unknown stream below the channels' numbers still closes the link`() = test {
        val request = async { closedReason { session.peer.request("info.get") } }
        agent.expectMagic()
        agent.message()
        agent.raw(data(ProtocolConstants.STREAM_LAST, "files?", last = true))
        assertTrue(request.await().isNotEmpty())
        assertTrue(agent.closedByPhone())
    }

    @Test
    fun `an answer to channel-open that names no channel's stream closes the link`() = test {
        for (stream in listOf("7", "\"32768\"", "65536")) {
            val (phone, other) = MemoryLink.pair()
            val one = AgentSession(phone, scope)
            val end = AgentEnd(other)
            val sink = RecordedChannel()
            val open = async { closedReason { one.peer.requestChannel(openParams(), sink) } }
            end.expectMagic()
            end.message()
            end.send("""{"id":1,"result":{"stream":$stream,"maxMessage":4096,"credit":16,"version":"1.0.0"}}""")
            assertTrue(stream, open.await().isNotEmpty())
            assertTrue(stream, end.closedByPhone())
            assertNull(stream, sink.heard.poll())
        }
    }

    @Test
    fun `a channel's message is one DATA frame with the last flag, and the link ending ends the channel`() = test {
        val sink = RecordedChannel()
        val open = async { session.peer.requestChannel(openParams(), sink) }
        agent.expectMagic()
        agent.message()
        agent.send(answer32768)
        open.await()
        session.peer.channelMessage(32768, "hi".encodeToByteArray())
        val frame = agent.frame()
        assertEquals(FrameCodec.TYPE_DATA, frame.type)
        assertEquals(FrameCodec.FLAG_LAST, frame.flags)
        assertEquals(32768, frame.stream)
        assertEquals("hi", frame.payload.decodeToString())
        for (bad in listOf(ByteArray(0), ByteArray(ProtocolConstants.CHANNEL_MAX_MESSAGE + 1))) {
            assertTrue(runCatching { session.peer.channelMessage(32768, bad) }.exceptionOrNull() is IllegalArgumentException)
        }
        assertTrue(runCatching { session.peer.channelMessage(7, byteArrayOf(1)) }.exceptionOrNull() is IllegalArgumentException)

        assertEquals("opened 32768", sink.next())
        agent.close()
        assertEquals("link ended", sink.next())
        assertTrue(session.peer.openChannels().isEmpty())
    }

    @Test
    fun `a channel answered after its asker gave up is closed at once, and its frames are dropped`() = test {
        val sink = RecordedChannel(wanted = false)
        val open = async { session.peer.requestChannel(openParams(), sink) }
        agent.expectMagic()
        agent.message()
        open.cancel()
        open.join()
        agent.raw(msg(answer32768) + data(32768, "unwanted", last = true))
        assertEquals("""{"event":"channel.close","params":{"stream":32768,"reason":"closed"}}""", agent.message().toString())
        assertTrue(agent.pingAnswered())
        assertEquals("opened 32768", sink.next())
        assertNull(sink.heard.poll())
        assertTrue(session.peer.openChannels().isEmpty())
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

    @Test
    fun `with a silence set, what comes after a longer gap is not handled, nothing is written, and the link ends`() = test {
        // A peer by itself, with no keep-alive to race it, on a clock the test moves by hand.
        val clock = AtomicLong(1_000)
        val (phone, other) = MemoryLink.pair()
        val handled = AtomicInteger()
        val peer = Peer(
            phone,
            dialled = true,
            handler = object : PeerHandler {
                override suspend fun onRequest(peer: Peer, id: Long, method: String, params: JsonObject) {
                    handled.incrementAndGet()
                    peer.respond(id, JsonObject(emptyMap()))
                }

                override suspend fun onEvent(peer: Peer, name: String, seq: Long?, params: JsonObject) = Unit
            },
            now = clock::get,
        )
        peer.silenceMs = 30_000
        val ended = async { peer.run() }
        val end = AgentEnd(other)

        // Inside the silence: answered.
        clock.addAndGet(29_000)
        end.send("""{"id":1,"method":"ping","params":{}}""")
        end.expectMagic()
        assertEquals("""{"id":1,"result":{}}""", end.message().toString())

        // A gap of 31 s: the ping is not handled, and the next thing the agent's end reads is the link's end.
        clock.addAndGet(31_000)
        end.send("""{"id":2,"method":"ping","params":{}}""")

        assertTrue("the phone wrote into a link the agent had given up", end.closedByPhone())
        assertEquals(WENT_SILENT, ended.await())
        assertEquals(1, handled.get())
    }
}

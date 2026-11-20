// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.channels

import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.data.jobs.FakeChannel
import com.nbtapps.store.data.link.AgentEnd
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.ChannelSink
import com.nbtapps.store.data.link.FrameCodec
import com.nbtapps.store.data.link.MemoryLink
import com.nbtapps.store.data.link.Peer
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.data.link.ProtocolException
import com.nbtapps.store.data.link.RemoteException
import com.nbtapps.store.data.link.StreamSink
import java.lang.management.ManagementFactory
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlin.random.Random
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.JsonObject
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** One channel over a link, with the agent's end driven by hand: credits both ways, pauses, and every way it ends. */
class ChannelLinkTest {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val sessions = CopyOnWriteArrayList<AgentSession>()

    @After
    fun tearDown() {
        sessions.forEach { it.close("the test is over") }
        scope.cancel()
    }

    // On the shared pool, not the test's own thread: the agent's end is read with blocking calls, and what the
    // phone's side has started must run meanwhile.
    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTimeout(30_000) { block() } }

    /** A link with nothing said on it yet: the phone's session and the agent's end. */
    private class Rig(val session: AgentSession, val agent: AgentEnd)

    private fun rig(): Rig {
        val (phone, other) = MemoryLink.pair()
        return Rig(AgentSession(phone, scope).also { sessions += it }, AgentEnd(other))
    }

    private fun data(text: String, last: Boolean = true, stream: Int = STREAM) = data(text.encodeToByteArray(), last, stream)

    private fun data(bytes: ByteArray, last: Boolean = true, stream: Int = STREAM) =
        FrameCodec.encode(FrameCodec.TYPE_DATA, if (last) FrameCodec.FLAG_LAST else 0, stream, bytes)

    private fun msg(json: String) = FrameCodec.encode(FrameCodec.TYPE_MSG, 0, 0, json.encodeToByteArray())

    private fun answer(id: Any?, credit: Int = ProtocolConstants.CHANNEL_CREDIT, stream: Int = STREAM) =
        """{"id":$id,"result":{"stream":$stream,"maxMessage":4096,"credit":$credit,"version":"1.2.3"}}"""

    private fun AgentEnd.event(name: String, params: String = "{}") = send("""{"event":"$name","params":$params}""")

    private fun closeEvent(reason: String, stream: Int = STREAM) = """{"event":"channel.close","params":{"stream":$stream,"reason":"$reason"}}"""

    private fun creditEvent(messages: Int, stream: Int = STREAM) = """{"event":"channel.credit","params":{"stream":$stream,"messages":$messages}}"""

    /** The next frame from the phone, as text: "data STREAM TEXT", with " not last" for a frame with no last flag, or the message. */
    private fun AgentEnd.next(): String {
        val frame = frame()
        if (frame.type != FrameCodec.TYPE_DATA) return frame.payload.decodeToString()
        return "data ${frame.stream} ${frame.payload.decodeToString()}${if (frame.last) "" else " not last"}"
    }

    /** Whether the link still serves the agent, with nothing from the phone before the answer: a ping is answered. */
    private fun AgentEnd.pingAnswered(id: Int = 900): Boolean {
        send("""{"id":$id,"method":"ping","params":{}}""")
        return next() == """{"id":$id,"result":{}}"""
    }

    /** Opens `echo` of a made-up package, the agent granting [credit]. The agent's end has read the request. */
    private suspend fun CoroutineScope.open(rig: Rig, credit: Int = ProtocolConstants.CHANNEL_CREDIT): ChannelLink {
        val link = async { rig.session.openChannel("org.example.chan", "echo") }
        rig.agent.expectMagic()
        val request = rig.agent.message()
        assertEquals("\"channel.open\"", request["method"].toString())
        assertEquals("""{"package":"org.example.chan","channel":"echo","credit":16}""", request["params"].toString())
        rig.agent.send(answer(request["id"], credit))
        return link.await()
    }

    private suspend fun ChannelLink.text(): String? = receive()?.decodeToString()

    private suspend fun failure(block: suspend () -> Unit): Throwable? = try {
        block()
        null
    } catch (e: Exception) {
        e
    }

    @Test
    fun `messages go both ways in order, one DATA frame each, and the channel has what the agent answered`() = test {
        val rig = rig()
        val link = open(rig)
        assertEquals(STREAM, link.stream)
        assertEquals(ProtocolConstants.CHANNEL_MAX_MESSAGE, link.maxMessage)
        assertEquals("1.2.3", link.version)
        assertEquals("org.example.chan", link.packageId)
        assertEquals("echo", link.channel)
        assertEquals(ChannelState.Open, link.state.value)

        link.send("one".encodeToByteArray())
        link.send("two".encodeToByteArray())
        assertEquals("data $STREAM one", rig.agent.next())
        assertEquals("data $STREAM two", rig.agent.next())
        rig.agent.raw(data("three") + data("four"))
        assertEquals("three", link.text())
        assertEquals("four", link.text())

        // The largest message goes as one frame; an empty one and a larger one are refused here.
        link.send(ByteArray(ProtocolConstants.CHANNEL_MAX_MESSAGE) { it.toByte() })
        val largest = rig.agent.frame()
        assertEquals(ProtocolConstants.CHANNEL_MAX_MESSAGE, largest.payload.size)
        assertTrue(largest.last)
        assertTrue(failure { link.send(ByteArray(0)) } is IllegalArgumentException)
        assertTrue(failure { link.send(ByteArray(ProtocolConstants.CHANNEL_MAX_MESSAGE + 1)) } is IllegalArgumentException)
        assertEquals(ChannelState.Open, link.state.value)
    }

    @Test
    fun `the sender waits with no credit left, and goes on with each channel-credit`() = test {
        val rig = rig()
        val link = open(rig, credit = 2)
        link.send("a".encodeToByteArray())
        link.send("b".encodeToByteArray())
        val third = async { link.send("c".encodeToByteArray()) }
        assertEquals("data $STREAM a", rig.agent.next())
        assertEquals("data $STREAM b", rig.agent.next())
        delay(300)
        assertFalse("sent with no credit", third.isCompleted)
        assertTrue(rig.agent.pingAnswered())

        rig.agent.send(creditEvent(1))
        third.await()
        assertEquals("data $STREAM c", rig.agent.next())

        val more = async { repeat(4) { link.send("m$it".encodeToByteArray()) } }
        rig.agent.send(creditEvent(3))
        assertEquals(listOf("m0", "m1", "m2").map { "data $STREAM $it" }, List(3) { rig.agent.next() })
        delay(200)
        assertFalse(more.isCompleted)
        rig.agent.send(creditEvent(1))
        more.await()
        assertEquals("data $STREAM m3", rig.agent.next())
    }

    @Test
    fun `the agent is granted 8 more as each 8 messages are taken, and not before`() = test {
        val rig = rig()
        val link = open(rig)
        rig.agent.raw((0 until 16).map { data("m$it") }.reduce(ByteArray::plus))
        repeat(7) { assertEquals("m$it", link.text()) }
        // Seven taken: nothing granted yet, so the first thing to come is the ping's answer.
        assertTrue(rig.agent.pingAnswered())
        assertEquals("m7", link.text())
        assertEquals("""{"event":"channel.credit","params":{"stream":$STREAM,"messages":8}}""", rig.agent.next())
        repeat(8) { assertEquals("m${it + 8}", link.text()) }
        assertEquals("""{"event":"channel.credit","params":{"stream":$STREAM,"messages":8}}""", rig.agent.next())

        // Granted 16 again in all: 16 more are within the rules.
        rig.agent.raw((16 until 32).map { data("m$it") }.reduce(ByteArray::plus))
        repeat(16) { assertEquals("m${it + 16}", link.text()) }
        assertEquals(ChannelState.Open, link.state.value)
    }

    @Test
    fun `the agent breaking the channel's rules closes the channel as its fault, tells it protocol, and leaves the link`() = test {
        val faults = listOf(
            "a message over the credit the phone gave" to (0..16).map { data("m$it") }.reduce(ByteArray::plus),
            "a DATA frame with no last flag" to data("m", last = false),
            "an empty message" to data(ByteArray(0)),
            "a message of 4097 bytes" to data(ByteArray(4097)),
            "a channel.credit of 0 messages" to msg(creditEvent(0)),
        )
        for ((what, bytes) in faults) {
            val rig = rig()
            val link = open(rig)
            rig.agent.raw(bytes)
            assertEquals(what, ChannelState.Closed(ChannelEnd.AgentFault(what)), link.state.first { it is ChannelState.Closed })
            assertEquals(what, closeEvent("protocol"), rig.agent.next())
            // What came within the rules before it is still there; nothing after it is taken.
            rig.agent.raw(data("after"))
            assertTrue(what, rig.agent.pingAnswered())
            val held = ArrayList<String>()
            while (true) held += link.text() ?: break
            assertEquals(what, if (what.startsWith("a message over")) (0..15).map { "m$it" } else emptyList<String>(), held)
            assertTrue(rig.session.peer.isOpen)
            assertTrue(failure { link.send(byteArrayOf(1)) } is ChannelClosedException)
        }
    }

    @Test
    fun `a paused channel sends nothing until it is resumed, and still takes what comes`() = test {
        val rig = rig()
        val link = open(rig)
        rig.agent.event("channel.paused")
        link.state.first { it == ChannelState.Paused }
        val held = async { link.send("held".encodeToByteArray()) }
        delay(300)
        assertFalse("sent while paused", held.isCompleted)
        rig.agent.raw(data("in"))
        assertEquals("in", link.text())
        assertTrue(rig.agent.pingAnswered())

        rig.agent.event("channel.resumed")
        held.await()
        assertEquals("data $STREAM held", rig.agent.next())
        assertEquals(ChannelState.Open, link.state.value)
    }

    @Test
    fun `each reason the agent closes a channel for ends it so, after what came before, and wakes a waiting sender`() = test {
        for (reason in listOf("closed", "off", "package-changed", "protocol", "a-later-reason")) {
            val rig = rig()
            val link = open(rig, credit = 0)
            val sender = async { failure { link.send("waits".encodeToByteArray()) } }
            delay(100)
            rig.agent.raw(data("last words") + msg(closeEvent(reason)) + data("crossed the close"))
            assertEquals(reason, "last words", link.text())
            assertNull(reason, link.text())
            assertEquals(reason, ChannelState.Closed(ChannelEnd.ByAgent(reason)), link.state.value)
            assertEquals(reason, ChannelEnd.ByAgent(reason), (sender.await() as ChannelClosedException).end)
            // Nothing is said back, and the link goes on.
            assertTrue(reason, rig.agent.pingAnswered())
            assertTrue(rig.session.peer.openChannels().isEmpty())
            link.close()
            assertTrue(reason, rig.agent.pingAnswered(901))
        }
    }

    @Test
    fun `closing here tells the agent once, drops what was held, and ends what waits`() = test {
        val rig = rig()
        val link = open(rig, credit = 0)
        rig.agent.raw(data("never taken"))
        assertTrue(rig.agent.pingAnswered())
        val sender = async { failure { link.send("waits".encodeToByteArray()) } }
        delay(100)
        link.close()
        link.close()
        assertEquals("""{"event":"channel.close","params":{"stream":$STREAM,"reason":"closed"}}""", rig.agent.next())
        assertTrue("told twice", rig.agent.pingAnswered())
        assertEquals(ChannelEnd.ByApp, (sender.await() as ChannelClosedException).end)
        assertNull(link.receive())
        assertEquals(ChannelState.Closed(ChannelEnd.ByApp), link.state.value)
        assertTrue(failure { link.send(byteArrayOf(1)) } is ChannelClosedException)
        // What the agent sent before it heard is dropped.
        rig.agent.raw(data("crossed"))
        assertTrue(rig.agent.pingAnswered(901))
        assertNull(link.receive())
    }

    @Test
    fun `a lost link ends the channel with the link's reason, and wakes a receiver and a sender`() = test {
        val rig = rig()
        val link = open(rig, credit = 0)
        val receiver = async { link.receive() }
        val sender = async { failure { link.send("waits".encodeToByteArray()) } }
        delay(100)
        rig.agent.close()
        assertNull(receiver.await())
        val end = (link.state.first { it is ChannelState.Closed } as ChannelState.Closed).end
        assertTrue(end.toString(), end is ChannelEnd.LinkLost && end.reason.isNotEmpty())
        assertEquals(end, (sender.await() as ChannelClosedException).end)
        link.close()
    }

    /**
     * The peer's entry for the channel [stream], held from a thread of its own until [close]: whatever ends the channel
     * meanwhile stops where it takes the channel off the peer, part way through ending it.
     */
    private class HeldEntry(peer: Peer, stream: Int) : AutoCloseable {
        private val holding = CountDownLatch(1)
        private val release = CountDownLatch(1)
        private val thread = Thread({
            @Suppress("UNCHECKED_CAST")
            val channels = Peer::class.java.getDeclaredField("channels").apply { isAccessible = true }.get(peer) as ConcurrentHashMap<Int, ChannelSink>
            // The map holds the entry's lock while this runs.
            channels.computeIfPresent(stream) { _, sink ->
                holding.countDown()
                release.await(20, TimeUnit.SECONDS)
                sink
            }
        }, "held-entry").apply { start() }

        init {
            assertTrue("the entry is held", holding.await(5, TimeUnit.SECONDS))
        }

        /** Returns once another thread waits for the entry. */
        suspend fun awaitWaiter() {
            val threads = ManagementFactory.getThreadMXBean()
            while (threads.dumpAllThreads(false, false).none { it.lockOwnerName == thread.name }) delay(5)
        }

        override fun close() {
            release.countDown()
            thread.join()
        }
    }

    @Test
    fun `a receiver told of the end finds the state closed with why, however far the ending has got`() = test {
        // The agent's close, read up to where the channel is taken off the peer.
        run {
            val rig = rig()
            val link = open(rig)
            HeldEntry(rig.session.peer, STREAM).use { held ->
                rig.agent.raw(data("last words") + msg(closeEvent("off")))
                held.awaitWaiter()
                assertEquals("last words", link.text())
                assertNull(link.receive())
                assertEquals(ChannelState.Closed(ChannelEnd.ByAgent("off")), link.state.value)
                // A close meanwhile returns with the state saying so too.
                link.close()
                assertEquals(ChannelState.Closed(ChannelEnd.ByAgent("off")), link.state.value)
            }
            assertTrue(rig.agent.pingAnswered())
        }
        // This side's close, as far.
        run {
            val rig = rig()
            val link = open(rig)
            HeldEntry(rig.session.peer, STREAM).use { held ->
                val closing = async { link.close() }
                held.awaitWaiter()
                assertNull(link.receive())
                assertEquals(ChannelState.Closed(ChannelEnd.ByApp), link.state.value)
                link.close()
                assertEquals(ChannelState.Closed(ChannelEnd.ByApp), link.state.value)
                assertFalse(closing.isCompleted)
                held.close()
                closing.await()
            }
            assertEquals(closeEvent("closed"), rig.agent.next())
            assertTrue(rig.agent.pingAnswered())
        }
    }

    /**
     * The link ends while the channel's sender is mid-way: the peer tells a stream of another request of its end
     * before it tells the channel, and that stream's sink sends on the channel then.
     */
    @Test
    fun `a sender that finds the link gone finds the state closed with the link's reason`() = test {
        val rig = rig()
        val link = open(rig)
        var seen: Pair<Throwable?, ChannelState>? = null
        val sink = object : StreamSink {
            override fun onData(bytes: ByteArray, last: Boolean) = Unit

            override fun onEnd(code: String?, message: String) {
                seen = runBlocking { failure { link.send("late".encodeToByteArray()) } to link.state.value }
            }
        }
        val fetching = async { rig.session.peer.requestStream("files.fetch", JsonObject(emptyMap()), sink) }
        rig.agent.send("""{"id":${rig.agent.message()["id"]},"result":{"stream":1}}""")
        fetching.await()
        rig.session.close("gone")
        val (failed, state) = seen ?: error("the stream was not told of the end")
        assertEquals(ChannelState.Closed(ChannelEnd.LinkLost("gone")), state)
        assertEquals(ChannelEnd.LinkLost("gone"), (failed as ChannelClosedException).end)
        assertEquals(state, link.state.value)
    }

    /**
     * The answer to `channel.open` and the channel's first messages and its close can come in one read of the link.
     * They are taken up as the answer is read, by the reading coroutine, not later by the coroutine that waited for
     * the answer: that one is held here until the agent has seen a ping answered, which is read after them all.
     */
    @Test
    fun `the channel's first frames are taken however soon they follow the answer, before its opener has gone on`() = test {
        val rig = rig()
        val executor = Executors.newSingleThreadExecutor()
        val holder = executor.asCoroutineDispatcher()
        try {
            val opening = async(holder) { rig.session.openChannel("org.example.chan", "echo") }
            rig.agent.expectMagic()
            val request = rig.agent.message()
            // The opener has let go of the link's write lock once something else has been written: it does so on its
            // own thread, which is taken next.
            assertTrue(rig.agent.pingAnswered(899))
            // The opener's thread is kept busy: it cannot take up anything of the answer until it is let go. The answer
            // goes once the thread is taken, so that the opener has stopped to wait for it and cannot run on.
            val taken = CountDownLatch(1)
            val release = CountDownLatch(1)
            executor.execute {
                taken.countDown()
                release.await(20, TimeUnit.SECONDS)
            }
            assertTrue("the opener's thread is taken", taken.await(5, TimeUnit.SECONDS))
            rig.agent.raw(
                msg(answer(request["id"])) + data("first") + data("second") + msg(closeEvent("closed")) +
                    msg("""{"id":900,"method":"ping","params":{}}"""),
            )
            assertEquals("""{"id":900,"result":{}}""", rig.agent.next())
            assertFalse("the opener went on", opening.isCompleted)
            release.countDown()
            val link = opening.await()
            assertEquals("first", link.text())
            assertEquals("second", link.text())
            assertNull(link.receive())
            assertEquals(ChannelState.Closed(ChannelEnd.ByAgent("closed")), link.state.value)
        } finally {
            holder.close()
        }
    }

    @Test
    fun `an opener that gives up leaves nothing open, and the channel a late answer opens is closed at once`() = test {
        val rig = rig()
        val opening = async { rig.session.openChannel("org.example.chan", "echo") }
        rig.agent.expectMagic()
        val request = rig.agent.message()
        opening.cancel()
        opening.join()
        rig.agent.raw(msg(answer(request["id"])) + data("unwanted"))
        assertEquals("""{"event":"channel.close","params":{"stream":$STREAM,"reason":"closed"}}""", rig.agent.next())
        assertTrue(rig.agent.pingAnswered())
        assertTrue(rig.session.peer.openChannels().isEmpty())
    }

    @Test
    fun `a refusal comes out with the agent's code, and an answer this app cannot read closes what it opened`() = test {
        val rig = rig()
        val refused = async { failure { rig.session.openChannel("org.example.chan", "gated") } }
        rig.agent.expectMagic()
        rig.agent.send("""{"id":${rig.agent.message()["id"]},"error":{"code":"off","message":"switched off"}}""")
        assertEquals("off", (refused.await() as RemoteException).code)

        val unreadable = async { failure { rig.session.openChannel("org.example.chan", "echo") } }
        rig.agent.send("""{"id":${rig.agent.message()["id"]},"result":{"stream":$STREAM,"maxMessage":4096,"credit":16}}""")
        assertTrue(unreadable.await() is ProtocolException)
        assertEquals("""{"event":"channel.close","params":{"stream":$STREAM,"reason":"protocol"}}""", rig.agent.next())
        assertTrue(rig.session.peer.openChannels().isEmpty())
        assertTrue(rig.agent.pingAnswered())
    }

    /** The test stand-in for the agent ([FakeAgent]) has an echo channel, which later steps test against. */
    @Test
    fun `hundreds of messages of every size go through the fake agent's echo and come back as they went`() = test {
        val (phone, other) = MemoryLink.pair()
        val fake = FakeAgent(other, "000102030405060708090a0b0c0d0e0f")
        fake.capabilities = """["${ProtocolConstants.CAPABILITY_CHANNELS}"]"""
        fake.addChannel(FakeChannel("org.nbtapp.chan", "echo"))
        val session = AgentSession(phone, scope).also { sessions += it }
        assertTrue(ProtocolConstants.CAPABILITY_CHANNELS in session.client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9").capabilities)

        val echo = session.openChannel("org.nbtapp.chan", "echo")
        val random = Random(19)
        val sent = (0 until 300).map { random.nextBytes(it * 37 % ProtocolConstants.CHANNEL_MAX_MESSAGE + 1) }
        val writer = async { sent.forEach { echo.send(it) } }
        for (message in sent) assertArrayEquals(message, echo.receive())
        writer.await()
        assertEquals(ChannelState.Open, echo.state.value)
        echo.close()
        val close = generateSequence { fake.channelEvents.poll(5, TimeUnit.SECONDS) }.first { it["event"].toString() == "\"channel.close\"" }
        assertEquals("""{"stream":$STREAM,"reason":"closed"}""", close["params"].toString())
        assertEquals(300, fake.channelMessages.getValue(STREAM).size)

        // Not an agent with channels: refused as the real one refuses.
        fake.capabilities = "[]"
        assertEquals("unsupported", (failure { session.openChannel("org.nbtapp.chan", "echo") } as RemoteException).code)
    }

    private companion object {
        const val STREAM = ProtocolConstants.CHANNEL_FIRST_STREAM
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.crypto.sha256Hex
import java.io.File
import java.io.RandomAccessFile
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.JsonObject
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The phone's answers to `blob.fetch`, with the agent's end of the link driven by hand. */
class BlobServerTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    @After
    fun tearDown() = scope.cancel()

    private val manifest = ByteArray(3000) { (it * 5).toByte() }
    private val archive = ByteArray(200_000) { (it * 31 + it / 7).toByte() }
    private val manifestHash = sha256Hex(manifest)
    private val archiveHash = sha256Hex(archive)

    private class Linked(val session: AgentSession, val agent: AgentEnd, val file: File)

    /** A link after `hello`, with one archive and its manifest on offer. */
    private suspend fun CoroutineScope.linked(maxFrame: Int = 1024, capacity: Int = Int.MAX_VALUE): Linked {
        val (phone, other) = MemoryLink.pair(capacity)
        val session = AgentSession(phone, scope)
        val agent = AgentEnd(other)
        val hello = async { session.client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9") }
        agent.answerHello(maxFrame)
        hello.await()
        val file = folder.newFile().apply { writeBytes(archive) }
        session.blobs.offer(mapOf(archiveHash to Blob.OfFile(file, archive.size.toLong()), manifestHash to Blob.OfBytes(manifest)))
        return Linked(session, agent, file)
    }

    // On the shared pool, not the test's own thread: the agent's end is read with blocking calls, and what the
    // phone's side has started must run meanwhile.
    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTimeout(30_000) { block() } }

    private fun AgentEnd.fetch(id: Int, hash: String, offset: Long, length: Long? = null) =
        send("""{"id":$id,"method":"blob.fetch","params":{"sha256":"$hash","offset":$offset${length?.let { ""","length":$it""" } ?: ""}}}""")

    private fun JsonObject.code() = (getValue("error") as JsonObject).getValue("code").toString().trim('"')

    private fun JsonObject.result() = getValue("result") as JsonObject

    /** Reads the DATA frames of [stream] up to the one flagged last, checking each on the way. */
    private fun AgentEnd.stream(stream: Int, maxFrame: Int): ByteArray {
        var out = ByteArray(0)
        while (true) {
            val frame = frame()
            assertEquals("a DATA frame", FrameCodec.TYPE_DATA, frame.type)
            assertEquals(stream, frame.stream)
            assertTrue(frame.payload.size in 1..maxFrame)
            out += frame.payload
            if (frame.last) return out
        }
    }

    @Test
    fun `a manifest is sent whole when no length is asked, in frames of the agreed size`() = test {
        val link = linked()
        link.agent.fetch(1, manifestHash, 0)
        val reply = link.agent.message()
        assertEquals("1", reply["id"].toString())
        assertEquals("""{"stream":1,"size":3000}""", reply.result().toString())

        val frames = ArrayList<Frame>()
        do frames += link.agent.frame() while (!frames.last().last)
        assertEquals(listOf(1024, 1024, 952), frames.map { it.payload.size })
        assertTrue(frames.all { it.stream == 1 && it.type == FrameCodec.TYPE_DATA })
        assertArrayEquals(manifest, frames.fold(ByteArray(0)) { all, frame -> all + frame.payload })
    }

    @Test
    fun `ranges of an archive come from the file, one stream after another`() = test {
        val link = linked()
        link.agent.fetch(1, archiveHash, 0, 100_000)
        assertEquals("""{"stream":1,"size":100000}""", link.agent.message().result().toString())
        val first = link.agent.stream(1, 1024)

        // The next range may be asked for the moment the last frame is in.
        link.agent.fetch(2, archiveHash, 100_000, 100_000)
        assertEquals("""{"stream":2,"size":100000}""", link.agent.message().result().toString())
        val second = link.agent.stream(2, 1024)
        assertArrayEquals(archive, first + second)

        // From an offset to the end, and the very last byte alone.
        link.agent.fetch(3, archiveHash, 199_000)
        assertEquals("""{"stream":3,"size":1000}""", link.agent.message().result().toString())
        assertArrayEquals(archive.copyOfRange(199_000, 200_000), link.agent.stream(3, 1024))
        link.agent.fetch(4, archiveHash, 199_999, 1)
        link.agent.message()
        assertArrayEquals(byteArrayOf(archive.last()), link.agent.stream(4, 1024))
    }

    @Test
    fun `what was not offered is not found, and a range outside the blob is refused`() = test {
        val link = linked()
        link.agent.fetch(1, "0".repeat(64), 0)
        assertEquals("not-found", link.agent.message().code())
        link.agent.fetch(2, archiveHash, 200_000)
        assertEquals("out-of-range", link.agent.message().code())
        link.agent.fetch(3, archiveHash, 199_999, 2)
        assertEquals("out-of-range", link.agent.message().code())
        link.agent.fetch(4, archiveHash, 0, 0)
        assertEquals("bad-request", link.agent.message().code())
        link.agent.send("""{"id":5,"method":"blob.fetch","params":{"sha256":"$archiveHash"}}""")
        assertEquals("bad-request", link.agent.message().code())
        link.agent.send("""{"id":6,"method":"blob.fetch","params":{"sha256":"$archiveHash","offset":-1}}""")
        assertEquals("bad-request", link.agent.message().code())

        // The offers of a newer plan replace the ones before, unless a job was started from those.
        link.session.blobs.offer(emptyMap())
        link.agent.fetch(7, archiveHash, 0, 10)
        assertEquals("not-found", link.agent.message().code())
    }

    @Test
    fun `a started job keeps its blobs whatever is planned next, until it has ended`() = test {
        val link = linked()
        link.session.blobs.jobStarted()
        link.session.blobs.offer(emptyMap())
        link.agent.fetch(1, archiveHash, 0, 10)
        assertEquals("""{"stream":1,"size":10}""", link.agent.message().result().toString())
        link.agent.stream(1, 1024)

        link.session.blobs.jobEnded()
        link.agent.fetch(2, archiveHash, 0, 10)
        assertEquals("not-found", link.agent.message().code())
    }

    @Test
    fun `a file that has gone from the phone is not found`() = test {
        val link = linked()
        link.file.delete()
        link.agent.fetch(1, archiveHash, 0, 10)
        assertEquals("not-found", link.agent.message().code())
    }

    @Test
    fun `a second fetch while a stream is open gets busy, and the stream goes on`() = test {
        // A link that holds little: the stream waits for the agent to read, so it is still open.
        val link = linked(capacity = 4)
        link.agent.fetch(1, archiveHash, 0, 100_000)
        assertEquals("""{"stream":1,"size":100000}""", link.agent.message().result().toString())
        link.agent.fetch(2, manifestHash, 0)

        var received = 0
        var busy = false
        var last = false
        while (!last || !busy) {
            val frame = link.agent.frame()
            if (frame.type == FrameCodec.TYPE_MSG) {
                val message = com.nbtapps.store.data.json.StrictJson.parseObject(frame.payload)
                assertEquals("2", message["id"].toString())
                assertEquals("busy", message.code())
                busy = true
            } else {
                received += frame.payload.size
                last = frame.last
            }
        }
        assertEquals(100_000, received)
    }

    @Test
    fun `stopping on the phone ends the stream with blob-error and no last frame`() = test {
        val link = linked(capacity = 4)
        link.agent.fetch(1, archiveHash, 0, 200_000)
        link.agent.message()
        assertFalse(link.agent.frame().last)

        val cancelled = async { link.session.blobs.cancel(link.session.peer) }
        var received = 0
        while (true) {
            val frame = link.agent.frame()
            if (frame.type == FrameCodec.TYPE_DATA) {
                assertFalse("the stream must not end as if it were whole", frame.last)
                received += frame.payload.size
                continue
            }
            val event = com.nbtapps.store.data.json.StrictJson.parseObject(frame.payload)
            assertEquals("\"blob.error\"", event["event"].toString())
            val params = event.getValue("params") as JsonObject
            assertEquals("1", params["stream"].toString())
            assertEquals("\"cancelled\"", params["code"].toString())
            break
        }
        assertTrue(cancelled.await())
        assertTrue(received < 200_000)

        // Nothing is open now: there is nothing more to stop, and a new fetch is served.
        assertFalse(link.session.blobs.cancel(link.session.peer))
        link.agent.fetch(2, manifestHash, 0, 5)
        assertEquals("""{"stream":2,"size":5}""", link.agent.message().result().toString())
    }

    @Test
    fun `an archive that can no longer be read ends its stream with an io-error`() = test {
        val link = linked(capacity = 4)
        link.agent.fetch(1, archiveHash, 0, 200_000)
        link.agent.message()
        link.agent.frame()
        // The stream is waiting for the agent to read. Meanwhile the file loses its second half.
        RandomAccessFile(link.file, "rw").use { it.setLength(50_000) }

        while (true) {
            val frame = link.agent.frame()
            if (frame.type == FrameCodec.TYPE_DATA) {
                assertFalse(frame.last)
                continue
            }
            val event = com.nbtapps.store.data.json.StrictJson.parseObject(frame.payload)
            assertEquals("\"blob.error\"", event["event"].toString())
            assertEquals("\"io-error\"", (event.getValue("params") as JsonObject)["code"].toString())
            break
        }
    }
}

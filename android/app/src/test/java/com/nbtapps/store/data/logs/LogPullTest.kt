// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.logs

import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.jobs.AGENT_PACKAGE_ID
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.FakeLog
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.Refusal
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.AgentFile
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.TcpEndpoint
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.zip.ZipFile
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.jsonPrimitive
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** Pulling a package's logs into a ZIP, against a stand-in agent that sends them as the real one does. */
class LogPullTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector()
    private val kept = AtomicInteger()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val share by lazy { folder.newFolder("share") }
    private val jobs by lazy {
        JobRepository(
            headUnits, connector, identity, MemoryPairingKeys(), PackageStore(folder.newFolder("packages")), { kept.incrementAndGet() }, scope,
            now = { 1_794_400_123_000L }, shareDir = share,
        )
    }

    private val radio = """{"id":"com.example.radio","name":"Radio","version":"1.4.0","key":"${"ab".repeat(32)}","flags":[],"logs":true}"""

    /** A log as a program writes one: lines that are much alike, so deflate shrinks it, over several frames. */
    private fun text(lines: Int, word: String) = (1..lines).joinToString("") { "line $it of the $word log, much like the one before\n" }.encodeToByteArray()

    private val big = FakeLog("\${data}/log/radio.log", text(400, "radio"), modified = 1_794_400_000)
    private val old = FakeLog("\${data}/log/radio-1.log", text(30, "older"), modified = 1_794_300_000)
    private val crash = FakeLog("/tmp/radio-crash.txt", "crashed\n".encodeToByteArray(), modified = 1_794_200_000)
    private val empty = FakeLog("\${tmp}/empty.log", ByteArray(0))

    init {
        connector.onConnect = ::setUp
    }

    @After
    fun tearDown() = scope.cancel()

    private fun setUp(agent: FakeAgent) {
        agent.capabilities = """["files"]"""
        agent.installed = "[$radio]"
        agent.logs["com.example.radio"] = listOf(big, old, crash, empty)
        agent.dataChunk = 300
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144))))
            block()
        }
    }

    private suspend fun connect() {
        assertTrue(jobs.connect("car").isSuccess)
    }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private suspend fun listed(): List<AgentFile> = jobs.listLogs("com.example.radio").getOrThrow().files

    private fun entries(zip: File): Map<String, ByteArray> = ZipFile(zip).use { file ->
        file.entries().toList().associate { it.name to file.getInputStream(it).readBytes() }
    }

    @Test
    fun `the files chosen are pulled into a ZIP, each whole and named after where it was, with an info file`() = test {
        connect()
        assertTrue((jobs.connection.value as ConnectionState.Connected).files)
        assertTrue((jobs.connection.value as ConnectionState.Connected).installed.single().logs)
        val files = listed()
        assertEquals(listOf(big.path, old.path, crash.path, empty.path), files.map { it.path })
        assertEquals(big.bytes.size.toLong(), files[0].size)

        val zip = jobs.pullLogs(LogPullRequest("com.example.radio", files), "0.4.0").getOrThrow()
        assertEquals(share, zip.parentFile)
        assertTrue(zip.name.startsWith("com.example.radio-") && zip.name.endsWith(".zip"))
        val got = entries(zip)
        assertEquals(
            setOf(
                "com.example.radio/data/log/radio.log", "com.example.radio/data/log/radio-1.log", "com.example.radio/system/tmp/radio-crash.txt",
                "com.example.radio/tmp/empty.log", "info.txt",
            ),
            got.keys,
        )
        assertTrue(big.bytes.contentEquals(got.getValue("com.example.radio/data/log/radio.log")))
        assertTrue(old.bytes.contentEquals(got.getValue("com.example.radio/data/log/radio-1.log")))
        assertTrue(crash.bytes.contentEquals(got.getValue("com.example.radio/system/tmp/radio-crash.txt")))
        assertEquals(0, got.getValue("com.example.radio/tmp/empty.log").size)
        // Dated as the head unit has the file.
        ZipFile(zip).use { assertEquals(1_794_400_000_000L, it.getEntry("com.example.radio/data/log/radio.log").time / 1000 * 1000) }

        val info = got.getValue("info.txt").decodeToString()
        assertTrue(info, info.contains("Logs of Radio (com.example.radio 1.4.0)"))
        assertTrue(info, info.contains("From the head unit \"Test car\", agent 1.0.0"))
        assertTrue(info, info.contains("with NBTApps 0.4.0"))
        assertTrue(info, info.contains("\${data}/log/radio.log: ${big.bytes.size} bytes, changed 2026-11-11T"))
        assertTrue(info, info.contains("which may be wrong"))
        // Nothing of the phone's own in it.
        assertFalse(info, info.contains("Pixel"))
        assertFalse(info, info.contains(identity.id()))

        val done = jobs.logPull.value as LogPullState.Done
        assertEquals(4, done.included)
        val sent = done.files.first { it.file.path == big.path }.sent
        assertTrue("deflate shrinks a log: $sent of ${big.bytes.size}", sent in 1 until big.bytes.size / 3)
        assertTrue("the service is asked to keep the process", kept.get() > 0)
        // One stream after another, each to its end: nothing was cancelled.
        assertTrue(connector.agent.requestsOf("files.cancel").isEmpty())
        assertTrue(connector.agent.requestsOf("files.fetch").all { it.getValue("compress").jsonPrimitive.content == "deflate" })
    }

    @Test
    fun `with a tail only the end of each larger file is asked for`() = test {
        connect()
        val files = listed()
        val zip = jobs.pullLogs(LogPullRequest("com.example.radio", files.take(2), tailBytes = 2000), "0.4.0").getOrThrow()
        assertTrue(old.bytes.size < 2000)
        val fetches = connector.agent.requestsOf("files.fetch")
        assertEquals((big.bytes.size - 2000).toString(), fetches[0].getValue("offset").jsonPrimitive.content)
        // A file that is no larger is asked for whole.
        assertNull(fetches[1]["offset"])
        val got = entries(zip)
        assertTrue(big.bytes.copyOfRange(big.bytes.size - 2000, big.bytes.size).contentEquals(got.getValue("com.example.radio/data/log/radio.log")))
        assertTrue(got.getValue("info.txt").decodeToString().contains("its last 2000 bytes of ${big.bytes.size}"))
    }

    @Test
    fun `a file that comes damaged is left out and said so, and the pull goes on`() = test {
        connector.onConnect = { setUp(it); it.damaged += big.path }
        connect()
        val zip = jobs.pullLogs(LogPullRequest("com.example.radio", listed().take(2)), "0.4.0").getOrThrow()
        val got = entries(zip)
        assertFalse(got.containsKey("com.example.radio/data/log/radio.log"))
        assertTrue(old.bytes.contentEquals(got.getValue("com.example.radio/data/log/radio-1.log")))
        val done = jobs.logPull.value as LogPullState.Done
        assertTrue(done.files.first().failure.orEmpty().startsWith("It came damaged"))
        assertTrue(got.getValue("info.txt").decodeToString().contains("Not in this file:\n  \${data}/log/radio.log: It came damaged"))
        assertTrue(jobs.connection.value is ConnectionState.Connected)
    }

    @Test
    fun `a file the agent cannot read to the end is left out, and the next one is pulled`() = test {
        connector.onConnect = { setUp(it); it.errorAfterFrames = 1 }
        connect()
        val done = jobs.pullLogs(LogPullRequest("com.example.radio", listed().take(2)), "0.4.0").getOrThrow().let { jobs.logPull.value as LogPullState.Done }
        assertEquals("The head unit couldn't read it to the end", done.files[0].failure)
        // The older one is shorter than a frame, so its stream has ended before any error would come.
        assertNull(done.files[1].failure)
        assertEquals(1, done.included)
    }

    @Test
    fun `a pull that is stopped cancels its stream and leaves no ZIP`() = test {
        connector.onConnect = { setUp(it); it.stallAfterFrames = 2 }
        connect()
        jobs.startPull(LogPullRequest("com.example.radio", listed()), "0.4.0")
        await(jobs.logPull, "bytes of the first file") { it is LogPullState.Pulling && it.bytes > 0 }
        jobs.cancelPull()
        await(jobs.logPull, "the pull stopped") { it is LogPullState.Cancelled }
        withTimeout(5_000) { while (connector.agent.requestsOf("files.cancel").isEmpty()) delay(10) }
        assertEquals("1", connector.agent.requestsOf("files.cancel").single().getValue("stream").jsonPrimitive.content)
        assertTrue(share.listFiles().orEmpty().none { it.name.endsWith(".zip") })
        // The link is as good as before.
        assertEquals(4, listed().size)
    }

    @Test
    fun `while a job runs the agent sends nothing, and the user is told to wait`() = test {
        connector.onConnect = { setUp(it); it.handlers["files.fetch"] = { throw Refusal("busy") } }
        connect()
        assertTrue(jobs.pullLogs(LogPullRequest("com.example.radio", listed()), "0.4.0").isFailure)
        assertEquals(LogPullState.Failed("com.example.radio", "Wait for the install to finish, then try again."), jobs.logPull.value)
    }

    @Test
    fun `an agent that cannot send files is asked for none`() = test {
        connector.onConnect = { setUp(it); it.capabilities = "[]" }
        connect()
        assertFalse((jobs.connection.value as ConnectionState.Connected).files)
        assertEquals("This head unit's agent can't send log files. Update the agent first.", jobs.listLogs("com.example.radio").exceptionOrNull()?.message)
        assertTrue(connector.agent.requestsOf("files.list").isEmpty())
    }

    @Test
    fun `each pull empties the share folder of the one before`() = test {
        connect()
        val files = listed()
        jobs.pullLogs(LogPullRequest("com.example.radio", files.take(1)), "0.4.0").getOrThrow()
        File(share, "left-over.txt").writeText("x")
        val second = jobs.pullLogs(LogPullRequest(AGENT_PACKAGE_ID, emptyList()), "0.4.0").getOrThrow()
        assertTrue(second.name.startsWith("org.nbtapp.agent-"))
        assertEquals(listOf(second.name), share.listFiles().orEmpty().map { it.name })
        // The agent's own logs, of which none were chosen: the ZIP says where it is from all the same.
        assertTrue(entries(second).getValue("info.txt").decodeToString().startsWith("Logs of the agent (org.nbtapp.agent 1.0.0)"))
    }

    @Test
    fun `a name in the ZIP keeps inside the package's folder whatever the path`() {
        assertEquals("p/data/log/a.log", LogPull.entryName("p", "\${data}/log/a.log"))
        assertEquals("p/app/state.txt", LogPull.entryName("p", "\${app}/state.txt"))
        assertEquals("p/tmp/crash-1.txt", LogPull.entryName("p", "\${tmp}/crash-1.txt"))
        assertEquals("p/system/var/log/x.log", LogPull.entryName("p", "/var/log/x.log"))
        assertEquals("org.nbtapp.agent/agent.log.1", LogPull.entryName("org.nbtapp.agent", "agent.log.1"))
        assertEquals("p/data/etc/passwd", LogPull.entryName("p", "\${data}/../../etc/passwd"))
        assertEquals("p/system/a_b_c", LogPull.entryName("p", "/a\\b:c"))
    }
}

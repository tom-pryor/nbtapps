// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.AgentBench
import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.CatalogueParser
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.packages.StoredPackage
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.AutostartReason
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.Plan
import com.nbtapps.store.model.RemoveTarget
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.model.VersionRange
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.UnixDomainSocketAddress
import java.nio.ByteBuffer
import java.nio.channels.SocketChannel
import java.nio.file.Files
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import kotlin.random.Random
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
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test

/**
 * The app's job repository against the real agent: the agent repo's `nbtagent`, working on a folder that
 * stands in for a head unit, reached over its local socket. Everything between the repository and the agent is
 * the code that ships; only the link is a test's.
 *
 * The agent runs in a Docker container ([AgentBench]), because it runs the packages' hook scripts. It needs the
 * agent repo (`headunit/`), Docker, and the bench image (`make bench-image` there). Without those the tests
 * are skipped. Nothing here goes near a car.
 */
class RealAgentTest {

    private val agentRepo = AgentBench.repo
    private val sampleRoot = File("src/debug/assets/catalogue/official")

    private lateinit var work: File
    private lateinit var root: File
    private lateinit var socket: File
    private var agent: Process? = null

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = SocketConnector()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Unit test phone"
    }
    // None is used: the agent trusts its local socket as it is, and asks for no `auth` on it. Pairing and `auth`
    // against the real agent are in `DebugTcpLinkTest`, over the link on which the agent asks for them.
    private val keys = MemoryPairingKeys()
    private lateinit var store: PackageStore
    private lateinit var jobs: JobRepository
    private val index by lazy { CatalogueParser.parseIndex(File(sampleRoot, "v1/index.json").readBytes()) }

    /** A link over the agent's local socket. With a delay, each large write waits first, as a slow link makes it. */
    private class SocketLink(path: File, private val delayMillis: Long) : Link {
        private val channel = SocketChannel.open(UnixDomainSocketAddress.of(path.toPath()))
        override val kind = "local"

        override val input = object : InputStream() {
            override fun read(): Int = throw UnsupportedOperationException()
            override fun read(buffer: ByteArray, offset: Int, length: Int): Int = channel.read(ByteBuffer.wrap(buffer, offset, length))
        }

        override val output = object : OutputStream() {
            override fun write(byte: Int) = write(byteArrayOf(byte.toByte()), 0, 1)
            override fun write(bytes: ByteArray, offset: Int, length: Int) {
                if (delayMillis > 0 && length > 4096) Thread.sleep(delayMillis)
                val buffer = ByteBuffer.wrap(bytes, offset, length)
                while (buffer.hasRemaining()) channel.write(buffer)
            }
        }

        override fun close() = channel.close()
    }

    private inner class SocketConnector : LinkConnector {
        @Volatile
        var delayMillis = 0L
        val links = CopyOnWriteArrayList<SocketLink>()

        override fun supports(endpoint: Endpoint) = true

        override suspend fun connect(endpoint: Endpoint): Link = SocketLink(socket, delayMillis).also { links += it }
    }

    @Before
    fun startAgent() {
        assumeTrue(AgentBench.MISSING, AgentBench.available)
        // A short path: a local socket's name has to fit in about a hundred bytes. The socket has a folder of its
        // own, because the agent's container is handed that folder.
        work = Files.createTempDirectory("nbt").toFile()
        root = File(work, "root").apply { mkdirs() }
        socket = File(work, "s/a")
        launchAgent()
        store = PackageStore(File(work, "phone"))
        jobs = JobRepository(headUnits, connector, identity, keys, store, {}, scope)
    }

    private fun launchAgent() {
        agent = AgentBench.start(root, socket, File(work, "agent.log"))
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
        while (!socket.exists()) {
            check(System.nanoTime() < deadline && agent!!.isAlive) { "the agent did not start:\n" + File(work, "agent.log").readText() }
            Thread.sleep(20)
        }
    }

    private fun stopAgent() {
        agent?.let { AgentBench.stop(it) }
        agent = null
    }

    @After
    fun tearDown() {
        scope.cancel()
        connector.links.forEach { runCatching { it.close() } }
        stopAgent()
        if (::work.isInitialized) work.deleteRecursively()
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(120_000) {
            headUnits.add(HeadUnit("bench", "Test bench", listOf(TcpEndpoint("local", 1))))
            store.load()
            block()
        }
    }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(30_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private fun connected() = jobs.connection.value as ConnectionState.Connected

    /** Puts a sample archive on the phone the way a download does: copied in, verified and checked against the index. */
    private suspend fun download(id: String, version: String): StoredPackage {
        val entry = index.packages.first { it.id == id }.versions.first { it.version == version }
        return File(sampleRoot, "packages/$id-$version.nbtpkg").inputStream().use {
            store.add(it, listOf(AgentVectors.packageKey), id to entry)
        }
    }

    /** What the app does for an install: resolve against the head unit, offer what that needs, and ask for the plan. */
    private suspend fun planInstall(id: String, version: String): Plan {
        val pkg = index.packages.first { it.id == id }
        val target = Resolver.Needed(pkg, pkg.versions.first { it.version == version })
        val needed = Resolver.resolve(index, target, connected().installed, connected().info.agentVersion).needed
        val offers = needed.map { store.find(it.pkg.id, it.version.version) ?: download(it.pkg.id, it.version.version) }
        return jobs.plan(listOf(InstallTarget(id, version)), emptyList(), offers).getOrThrow()
    }

    private suspend fun run(plan: Plan): JobView {
        assertTrue("the plan can't run: ${plan.missing} ${plan.problems}", plan.runnable)
        jobs.start(plan).getOrThrow()
        return requireNotNull(await(jobs.job, "the end of job ${plan.planId}") { it != null && it.planId == plan.planId && it.ended })
    }

    private fun installedNow() = connected().installed.map { "${it.id} ${it.version}" }

    private suspend fun awaitInstalled(vararg expected: String) {
        await(jobs.connection, "installed = ${expected.toList()}") { (it as? ConnectionState.Connected)?.installed?.map { p -> "${p.id} ${p.version}" } == expected.toList() }
    }

    @Test
    fun `sample packages install, upgrade and uninstall on the real agent`() = test {
        assertTrue(jobs.connect("bench").isSuccess)
        assertEquals(AgentBench.version, connected().info.agentVersion)
        assertTrue(connected().installed.isEmpty())
        assertEquals(32, headUnits.headUnit("bench").first()?.headUnitId?.length)

        // An install that brings its dependency along.
        val plan = planInstall("org.nbtapp.hello", "1.1.0")
        assertEquals(listOf("install org.nbtapp.runtime 2.1.0", "install org.nbtapp.hello 1.1.0"), plan.steps.map { "${it.action} ${it.id} ${it.version}" })
        assertEquals(listOf("runs-scripts"), plan.flags)
        assertTrue(plan.fetchBytes > 0)
        val installed = run(plan)
        assertEquals(JobState.SUCCEEDED, installed.job.state)
        assertEquals(listOf("done", "done"), installed.job.result?.steps?.map { it.result })
        assertTrue("the hook's output reaches the phone", installed.log.any { it.source == "postInstall" && it.text.contains("post-install: install org.nbtapp.hello 1.1.0") })
        awaitInstalled("org.nbtapp.hello 1.1.0", "org.nbtapp.runtime 2.1.0")
        assertTrue("the package's program is on the head unit", root.walkTopDown().any { it.name == "hello" && it.isFile })

        // An upgrade, which needs only the one archive now.
        val upgrade = planInstall("org.nbtapp.hello", "1.2.0")
        assertEquals(listOf("upgrade org.nbtapp.hello 1.2.0 from 1.1.0"), upgrade.steps.map { "${it.action} ${it.id} ${it.version} from ${it.oldVersion}" })
        assertEquals(JobState.SUCCEEDED, run(upgrade).job.state)
        awaitInstalled("org.nbtapp.hello 1.2.0", "org.nbtapp.runtime 2.1.0")

        // The same version again, and a lower one, are refused in the plan.
        assertEquals(listOf("already-installed"), planInstall("org.nbtapp.hello", "1.2.0").problems.map { it.code })
        assertEquals(listOf("downgrade"), planInstall("org.nbtapp.hello", "1.1.0").problems.map { it.code })

        // What something else depends on stays.
        val refused = jobs.plan(emptyList(), listOf(RemoveTarget("org.nbtapp.runtime")), emptyList()).getOrThrow()
        assertFalse(refused.runnable)
        assertNull(refused.planId)
        assertEquals(listOf("required-by"), refused.problems.map { it.code })

        // Uninstalling works from the head unit's own record: nothing is offered.
        val removal = jobs.plan(emptyList(), listOf(RemoveTarget("org.nbtapp.hello", deleteData = true), RemoveTarget("org.nbtapp.runtime")), emptyList()).getOrThrow()
        assertEquals(listOf("remove", "remove"), removal.steps.map { it.action })
        val removed = run(removal)
        assertEquals(JobState.SUCCEEDED, removed.job.state)
        assertTrue(removed.log.any { it.source == "preRemove" })
        awaitInstalled()
        assertFalse(root.walkTopDown().any { it.name == "hello" && it.isFile })

        // The last job is still on record for a phone that connects later.
        jobs.disconnect()
        assertTrue(jobs.connect("bench").isSuccess)
        assertEquals(removal.planId, jobs.job.value?.planId)
        assertEquals(JobState.SUCCEEDED, jobs.job.value?.job?.state)
    }

    @Test
    fun `plugins, system files and restart needs come out as the catalogue says`() = test {
        assertTrue(jobs.connect("bench").isSuccess)

        // The newest launcher needs a newer agent than this one, and the agent says so itself.
        download("org.nbtapp.launcher", "0.3.0")
        val tooNew = jobs.plan(listOf(InstallTarget("org.nbtapp.launcher", "0.3.0")), emptyList(), listOf(requireNotNull(store.find("org.nbtapp.launcher", "0.3.0")), download("org.nbtapp.runtime", "2.1.0"))).getOrThrow()
        assertEquals(listOf("agent-too-old"), tooNew.problems.map { it.code })

        // The app's own resolution picks the launcher that does fit, as a dependency of the log viewer.
        val plan = planInstall("org.nbtapp.logview", "0.8.0")
        assertEquals(
            listOf("org.nbtapp.runtime 2.1.0", "org.nbtapp.launcher 0.2.1", "org.nbtapp.logview 0.8.0"),
            plan.steps.map { "${it.id} ${it.version}" },
        )
        assertEquals(listOf("needs-reboot"), plan.flags)
        assertEquals("hmi-restart", plan.restart)
        val result = run(plan)
        assertEquals(JobState.SUCCEEDED, result.job.state)
        assertEquals("hmi-restart", result.job.result?.restart)
        await(jobs.connection, "the restart need") { (it as? ConnectionState.Connected)?.info?.restartNeeded == "hmi-restart" }
        assertTrue(jobs.restart("hmi").isSuccess)

        // A package that writes outside its own folders is flagged, and asks for a reboot.
        val tweaks = planInstall("org.nbtapp.menutweaks", "1.0.1")
        assertEquals(listOf("modifies-system", "needs-reboot"), tweaks.flags)
        assertEquals("required", tweaks.restart)
        assertEquals(JobState.SUCCEEDED, run(tweaks).job.state)
        assertTrue(root.walkTopDown().any { it.name == "menu-order.conf" })

        // Every other sample package installs too.
        for ((id, version) in listOf("org.nbtapp.tzdata" to "2026.1.0", "org.nbtapp.tzdata" to "2026.2.0", "org.nbtapp.btterm" to "0.4.0", "org.nbtapp.hello" to "1.2.0")) {
            assertEquals("$id $version", JobState.SUCCEEDED, run(planInstall(id, version)).job.state)
        }
        await(jobs.connection, "seven packages") { (it as? ConnectionState.Connected)?.installed?.size == 7 }
    }

    /**
     * The way a user goes who wants the agent started with the car: the agent says what it lacks, the catalogue
     * has it, a job installs it, and the switch is then there to throw. The sample's plugin stands in for the real
     * one: what the agent goes by is that the package is installed, in a version that starts it.
     */
    @Test
    fun `the real agent is switched to start with the head unit once the plugin's package is installed`() = test {
        val home = File(root, "agent")

        // A bench agent is started by nothing, and says so.
        assertTrue(jobs.connect("bench").isSuccess)
        assertEquals(Autostart(on = false, available = false, reason = AutostartReason.NO_LAUNCHER), connected().autostart)
        assertEquals(AutostartReason.NO_LAUNCHER, (jobs.setAutostart(true).exceptionOrNull() as AgentRefusal).code)

        // With a starter in its folder, as an install on a car leaves one, what it lacks is the plugin.
        home.mkdirs()
        File(home, "launcher").writeText("#!/bin/sh\n")
        assertTrue(jobs.refresh().isSuccess)
        val lacking = requireNotNull(connected().autostart)
        assertEquals(AutostartReason.PLUGIN_MISSING, lacking.reason)
        assertFalse(lacking.available)
        val need = requireNotNull(lacking.needs)
        assertEquals("org.nbtapp.plugin", need.id)
        assertEquals(AutostartReason.PLUGIN_MISSING, (jobs.setAutostart(true).exceptionOrNull() as AgentRefusal).code)
        assertFalse(File(home, "autostart").exists())

        // The catalogue has the package the agent named, in a version inside the range it gave.
        val offered = requireNotNull(index.packages.first { it.id == need.id }.latest)
        assertTrue(VersionRange.matches(offered.version, need.version))
        assertEquals(JobState.SUCCEEDED, run(planInstall(need.id, offered.version)).job.state)

        // The job's end is what has the app ask again.
        await(jobs.connection, "the switch to be had") { (it as? ConnectionState.Connected)?.autostart?.available == true }
        assertEquals(Autostart(on = false, available = true), connected().autostart)

        assertTrue(jobs.setAutostart(true).isSuccess)
        assertEquals(Autostart(on = true, available = true), connected().autostart)
        assertTrue(File(home, "autostart").isFile)

        assertTrue(jobs.setAutostart(false).isSuccess)
        assertEquals(Autostart(on = false, available = true), connected().autostart)
        assertFalse(File(home, "autostart").exists())
    }

    /** Builds and signs a package too large to cross a slowed link at once, with the agent repo's tool and test key. */
    private suspend fun largePackage(): StoredPackage {
        val source = File(work, "big").apply { mkdirs() }
        File(source, "manifest.json").writeText(
            """{ "manifestVersion": 1, "id": "org.nbtapp.big", "name": "Big", "version": "1.0.0",
                 "install": { "files": [{ "src": "payload/blob.bin", "dest": "${'$'}{app}/blob.bin", "mode": "0644" }] } }""",
        )
        File(source, "payload").mkdirs()
        // Random bytes, so the archive stays as large as its payload.
        File(source, "payload/blob.bin").writeBytes(Random(7).nextBytes(3 * 1024 * 1024))
        val build = ProcessBuilder(
            "python3", File(agentRepo, "tools/nbtpkg.py").path, "build", source.path, "-o", work.path,
            "--key", File(agentRepo, "tests/keys/test-package.key").path,
        ).redirectErrorStream(true).start()
        val output = build.inputStream.readBytes().decodeToString()
        check(build.waitFor() == 0) { "the package tool failed: $output" }
        return File(work, "org.nbtapp.big-1.0.0.nbtpkg").inputStream().use { store.add(it, listOf(AgentVectors.packageKey)) }
    }

    @Test
    fun `a job stopped on the phone during the transfer ends cancelled, with nothing installed`() = test {
        assumeTrue("the agent repo's package tool is needed to build a large package", File(agentRepo, "tools/nbtpkg.py").isFile)
        val big = largePackage()
        connector.delayMillis = 20
        assertTrue(jobs.connect("bench").isSuccess)
        val plan = jobs.plan(listOf(InstallTarget("org.nbtapp.big", "1.0.0")), emptyList(), listOf(big)).getOrThrow()
        jobs.start(plan).getOrThrow()
        await(jobs.job, "the transfer to begin") { it?.job?.state == JobState.FETCHING }
        delay(200)

        assertTrue(jobs.cancel().isSuccess)

        val ended = requireNotNull(await(jobs.job, "the job to end") { it?.ended == true })
        assertEquals(JobState.CANCELLED, ended.job.state)
        assertEquals(listOf("skipped"), ended.job.result?.steps?.map { it.result })
        assertTrue(jobs.refresh().isSuccess)
        assertTrue(connected().installed.isEmpty())
        // The link is still good, and a new plan is taken.
        assertTrue(jobs.plan(listOf(InstallTarget("org.nbtapp.big", "1.0.0")), emptyList(), listOf(big)).getOrThrow().runnable)
    }

    @Test
    fun `a transfer cut off by a lost link goes on over a new one and installs`() = test {
        assumeTrue("the agent repo's package tool is needed to build a large package", File(agentRepo, "tools/nbtpkg.py").isFile)
        val big = largePackage()
        connector.delayMillis = 20
        assertTrue(jobs.connect("bench").isSuccess)
        val plan = jobs.plan(listOf(InstallTarget("org.nbtapp.big", "1.0.0")), emptyList(), listOf(big)).getOrThrow()
        assertEquals(big.size, plan.fetchBytes)
        jobs.start(plan).getOrThrow()
        await(jobs.job, "the transfer to begin") { it?.job?.state == JobState.FETCHING }
        delay(200)

        // The link goes, as when the phone leaves the car.
        connector.links.last().close()
        await(jobs.connection, "the lost link") { it is ConnectionState.Failed }
        assertTrue(requireNotNull(jobs.job.value).linkLost)

        // Back in range, over a link that is not slow. The agent kept what it had; the app names the plan again.
        connector.delayMillis = 0
        assertTrue(jobs.connect("bench").isSuccess)
        val ended = requireNotNull(await(jobs.job, "the job to end") { it?.ended == true })
        assertEquals(JobState.SUCCEEDED, ended.job.state)
        assertFalse(ended.linkLost)
        awaitInstalled("org.nbtapp.big 1.0.0")
        assertEquals(3L * 1024 * 1024, root.walkTopDown().first { it.name == "blob.bin" }.length())
    }

    @Test
    fun `a job does not outlast the agent, and the app says so`() = test {
        assumeTrue("the agent repo's package tool is needed to build a large package", File(agentRepo, "tools/nbtpkg.py").isFile)
        val big = largePackage()
        connector.delayMillis = 20
        assertTrue(jobs.connect("bench").isSuccess)
        val plan = jobs.plan(listOf(InstallTarget("org.nbtapp.big", "1.0.0")), emptyList(), listOf(big)).getOrThrow()
        jobs.start(plan).getOrThrow()
        await(jobs.job, "the transfer to begin") { it?.job?.state == JobState.FETCHING }
        delay(200)

        // The agent stops mid-transfer and starts again, as after the head unit lost power.
        stopAgent()
        await(jobs.connection, "the lost link") { it is ConnectionState.Failed }
        socket.delete()
        launchAgent()
        connector.delayMillis = 0
        assertTrue(jobs.connect("bench").isSuccess)

        // The new agent ended the job as failed: it had changed nothing. The app shows that, and does not resume.
        val view = requireNotNull(jobs.job.value)
        assertEquals(plan.planId, view.planId)
        assertEquals(JobState.FAILED, view.job.state)
        assertTrue(connected().installed.isEmpty())
        // Planned again, it installs.
        val again = jobs.plan(listOf(InstallTarget("org.nbtapp.big", "1.0.0")), emptyList(), listOf(big)).getOrThrow()
        assertEquals(JobState.SUCCEEDED, run(again).job.state)
    }
}

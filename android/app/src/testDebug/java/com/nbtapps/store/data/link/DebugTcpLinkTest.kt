// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.AgentBench
import com.nbtapps.store.AgentVectors
import com.nbtapps.store.CarScreen
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.PairingNeeded
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.LinkClosing
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import java.io.File
import java.net.ServerSocket
import java.nio.file.Files
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * The debug build's TCP link against the real agent: the agent repo's `nbtagent`, on a folder that stands in
 * for a head unit, listening with `--debug-tcp`. This is the path the app takes on an emulator, less the
 * emulator: the connector, the job repository and the agent are the code that runs there.
 *
 * The agent asks for pairing and `auth` on this link as it does on Bluetooth serial, so this is also where the
 * app's side of both meets the real agent's. The car's screen is stood in for by a controller on the agent's
 * local socket ([CarScreen]).
 *
 * The agent runs in a Docker container ([AgentBench]), which publishes the port on this machine's loopback
 * address. It needs the agent repo (`headunit/`), Docker, and the bench image (`make bench-image` there).
 * Without those the tests that need it are skipped, or fail under `NBT_REQUIRE_BENCH` ([AgentBench.need]).
 * Nothing here goes near a car.
 */
class DebugTcpLinkTest {

    private val sampleRoot = File("src/debug/assets/catalogue/official")

    private lateinit var work: File
    private var agent: Process? = null
    private var port = 0

    /** The agent's local socket: the way in for the car's side of pairing ([CarScreen]). */
    private lateinit var socket: File
    private var screen: CarScreen? = null
    private val keys = MemoryPairingKeys()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private val connector = DebugTcpLinkConnector()

    @Before
    fun setUp() {
        work = Files.createTempDirectory("nbt").toFile()
    }

    @After
    fun tearDown() {
        scope.cancel()
        screen?.close()
        agent?.let { AgentBench.stop(it) }
        work.deleteRecursively()
    }

    /**
     * Starts the agent on a free port of the loopback address, and reads the port from its log. The address the
     * agent names there is the one inside its container; on this machine the port is on 127.0.0.1.
     */
    private fun startAgent() {
        AgentBench.need(AgentBench.MISSING, AgentBench.available)
        val log = File(work, "agent.log")
        socket = File(work, "s/a")
        agent = AgentBench.start(File(work, "root"), socket, log, debugTcp = true)
        val said = Regex("""DEBUG TCP link at [0-9.]+:(\d+)""")
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
        while (true) {
            val found = if (log.exists()) said.find(log.readText()) else null
            if (found != null) {
                port = found.groupValues[1].toInt()
                return
            }
            check(System.nanoTime() < deadline && agent!!.isAlive) { "the agent did not start:\n" + log.readText() }
            Thread.sleep(20)
        }
    }

    @Test
    fun `reaches TCP endpoints only`() {
        assertTrue(connector.supports(TcpEndpoint("10.0.2.2", 7144)))
        assertFalse(connector.supports(SerialEndpoint("00:11:22:33:44:55", "BMW 12345")))
        assertTrue(connector.reachesTcp)
    }

    @Test
    fun `a port nobody listens on fails the connection, and the app says so`() = runBlocking(Dispatchers.Default) {
        // A port that was free a moment ago.
        val free = ServerSocket(0).use { it.localPort }
        val headUnits = InMemoryHeadUnitRepository()
        headUnits.add(HeadUnit("bench", "Test bench", listOf(TcpEndpoint("127.0.0.1", free))))
        val jobs = JobRepository(headUnits, connector, Phone, keys, PackageStore(File(work, "phone")), {}, scope)

        assertTrue(jobs.connect("bench").isFailure)
        assertTrue(jobs.connection.value is ConnectionState.Failed)
    }

    @Test
    fun `a host name that is not known fails with more than the name`() = runBlocking(Dispatchers.Default) {
        // `.invalid` is reserved: no resolver has it.
        val failure = runCatching { connector.connect(TcpEndpoint("nbt-bench.invalid", 7144)) }.exceptionOrNull()

        assertTrue("was: $failure", failure is java.io.IOException)
        assertTrue("was: ${failure?.message}", failure?.message.orEmpty().length > "nbt-bench.invalid".length)
    }

    /** Another code than [code], and as well formed. */
    private fun otherThan(code: String) = if (code == "000000") "000001" else "000000"

    @Test
    fun `the phone is paired by the code of the agent's pairing window, and a sample package then installs over TCP`() = runBlocking(Dispatchers.Default) {
        startAgent()
        withTimeout(60_000) {
            val headUnits = InMemoryHeadUnitRepository()
            headUnits.add(HeadUnit("bench", "Test bench", listOf(TcpEndpoint("127.0.0.1", port))))
            val store = PackageStore(File(work, "phone")).also { it.load() }
            val jobs = JobRepository(headUnits, connector, Phone, keys, store, {}, scope)
            val carScreen = CarScreen(socket).also { screen = it }

            // The agent answers `hello` and nothing more: this phone is not one it knows.
            assertTrue(jobs.connect("bench").exceptionOrNull() is PairingNeeded)
            assertEquals(ConnectionState.NeedsPairing("bench", TcpEndpoint("127.0.0.1", port), stale = false), jobs.connection.value)
            // Nobody has opened "Pair a phone" on the car.
            assertEquals("forbidden", (jobs.pair("123456").exceptionOrNull() as AgentRefusal).code)

            val code = carScreen.pairingCode()
            assertEquals(ProtocolConstants.PAIRING_CODE_DIGITS, code.length)
            assertEquals("rejected", (jobs.pair(otherThan(code)).exceptionOrNull() as AgentRefusal).code)
            assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)

            // The right code: the agent issues a key, the app keeps it and proves it, and the link is connected.
            assertTrue(jobs.pair(code).isSuccess)
            val agentId = requireNotNull(headUnits.headUnit("bench").first()?.headUnitId)
            assertEquals(setOf(agentId), keys.headUnitIds)
            assertEquals(listOf(Phone.id()), carScreen.phoneIds())
            // The code is used up.
            assertTrue(jobs.pair(code).isFailure)

            val connected = jobs.connection.value as ConnectionState.Connected
            assertEquals(AgentBench.version, connected.info.agentVersion)
            assertTrue(connected.installed.isEmpty())

            val runtime = File(sampleRoot, "packages/org.nbtapp.runtime-2.1.0.nbtpkg").inputStream().use {
                store.add(it, listOf(AgentVectors.packageKey))
            }
            val plan = jobs.plan(listOf(InstallTarget("org.nbtapp.runtime", "2.1.0")), emptyList(), listOf(runtime)).getOrThrow()
            assertTrue("the plan can't run: ${plan.missing} ${plan.problems}", plan.runnable)
            jobs.start(plan).getOrThrow()

            val done = jobs.job.first { it != null && it.planId == plan.planId && it.ended }
            assertEquals(JobState.SUCCEEDED, done?.job?.state)
            val installed = jobs.connection.first { (it as? ConnectionState.Connected)?.installed?.isNotEmpty() == true }
            assertEquals(listOf("org.nbtapp.runtime 2.1.0"), (installed as ConnectionState.Connected).installed.map { "${it.id} ${it.version}" })
            assertTrue("the package's files are in the agent's folder", File(work, "root").walkTopDown().any { it.isFile && it.name.startsWith("libnbt") })

            // A new link needs no code: the key is proved with `auth`, and the agent answers.
            jobs.disconnect()
            assertTrue(jobs.connect("bench").isSuccess)
            assertEquals(1, (jobs.connection.value as ConnectionState.Connected).installed.size)

            // "Forget all phones" on the car ends the phone's link. The agent says why first (`link.closing`), so
            // the app ends the link itself, knows what happened, and drops the key that is good for nothing now.
            assertEquals(1, carScreen.forgetAll())
            val forgotten = jobs.connection.first { it is ConnectionState.Failed } as ConnectionState.Failed
            assertEquals(AgentText.closing(LinkClosing.FORGOTTEN), forgotten.reason)
            assertEquals(emptySet<String>(), keys.headUnitIds)
            assertTrue(jobs.connect("bench").exceptionOrNull() is PairingNeeded)
            assertEquals(ConnectionState.NeedsPairing("bench", TcpEndpoint("127.0.0.1", port), stale = true), jobs.connection.value)

            // And it pairs again as it did the first time.
            assertTrue(jobs.pair(carScreen.pairingCode()).isSuccess)
            assertTrue(jobs.connection.value is ConnectionState.Connected)
            assertEquals(setOf(agentId), keys.headUnitIds)

            // The agent is stopped, as SIGTERM stops it on a head unit. It tells the phone that too, and the phone
            // keeps its key: it is good again when the agent is back.
            AgentBench.stop(requireNotNull(agent))
            agent = null
            val stopped = jobs.connection.first { it is ConnectionState.Failed } as ConnectionState.Failed
            assertEquals(AgentText.closing(LinkClosing.STOPPING), stopped.reason)
            assertEquals(setOf(agentId), keys.headUnitIds)
            assertTrue("the agent's log:\n" + log(), log().contains("told link.closing, stopping"))
        }
    }

    private fun log() = File(work, "agent.log").readText()

    private object Phone : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Unit test phone"
    }
}

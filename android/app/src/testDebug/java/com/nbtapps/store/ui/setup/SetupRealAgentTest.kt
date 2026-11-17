// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.lifecycle.SavedStateHandle
import com.nbtapps.store.AgentBench
import com.nbtapps.store.CarScreen
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.FakePhoneBluetooth
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.InMemoryPrereleaseRepository
import com.nbtapps.store.data.InMemorySetupRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.DefaultCatalogueRepository
import com.nbtapps.store.data.catalogue.FileCatalogueSource
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.AutostartStage
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.DebugTcpLinkConnector
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.Autostart
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.TcpEndpoint
import java.io.File
import java.nio.file.Files
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.job
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test

/**
 * The set-up for a new user against the real agent: the agent repo's `nbtagent`, on a folder that stands in for a
 * head unit, reached over the debug build's TCP link, on which the agent asks for pairing as it does on Bluetooth
 * serial. The view model, the job repository, the set-up for starting with the car and the agent are the code that
 * runs on a phone and a car; the car's screen is stood in for by a controller on the agent's local socket
 * ([CarScreen]), and the packages come from the sample catalogue the debug build has.
 *
 * The agent runs in a Docker container ([AgentBench]). It needs the agent repo (`headunit/`), Docker, and the bench
 * image (`make bench-image` there). Without those the test is skipped, or fails under `NBT_REQUIRE_BENCH`
 * ([AgentBench.need]). Nothing here goes near a car.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class SetupRealAgentTest {

    /** The view model's own connect runs on the main dispatcher: here it runs at once, as the app's does. */
    @get:Rule
    val mainDispatcherRule = MainDispatcherRule(main = Dispatchers.Unconfined)

    private val sampleRoot = File("src/debug/assets/catalogue/official")

    private lateinit var work: File
    private lateinit var root: File
    private lateinit var socket: File
    private var agent: Process? = null
    private var port = 0
    private var screen: CarScreen? = null

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val connector = DebugTcpLinkConnector()

    private object Phone : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Unit test phone"
    }

    /**
     * Starts the agent on a free port of the loopback address, and reads the port from its log. Its folder has a
     * starter in it, as an install on a car leaves one: without it nothing could start the agent with the car.
     */
    @Before
    fun startAgent() {
        AgentBench.need(AgentBench.MISSING, AgentBench.available)
        work = Files.createTempDirectory("nbt").toFile()
        root = File(work, "root")
        File(root, "agent").mkdirs()
        File(root, "agent/launcher").writeText("#!/bin/sh\n")
        val log = File(work, "agent.log")
        socket = File(work, "s/a")
        agent = AgentBench.start(root, socket, log, debugTcp = true)
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

    /**
     * Ends the view model and the link, and waits for them, before the agent's folder goes: see [MainDispatcherRule].
     * The agent stopping ends the link, which a cancel alone does not.
     */
    @After
    fun tearDown() {
        mainDispatcherRule.closeAtEnd {
            screen?.close()
            agent?.let { AgentBench.stop(it) }
        }
        mainDispatcherRule.end()
        if (::work.isInitialized) work.deleteRecursively()
    }

    private suspend fun SetupViewModel.await(what: String, predicate: (SetupUiState) -> Boolean): SetupUiState =
        withTimeoutOrNull(60_000) { uiState.first(predicate) } ?: throw AssertionError("never came: $what; last was ${uiState.value}")

    /**
     * A new user's whole way: the first question, a head unit at a network address as the debug build takes one,
     * the connect that finds the phone is not paired, the code from the car's screen, and the car set up to start
     * the agent, with the iDrive plugin installed for it; then iDrive's restart, and Done.
     */
    @Test
    fun `the set-up takes a new phone from the first question to a real agent that starts with the car`() = runBlocking(Dispatchers.Default) {
        withTimeout(120_000) {
            val headUnits = InMemoryHeadUnitRepository()
            val keys = MemoryPairingKeys()
            val store = PackageStore(File(work, "phone")).also { it.load() }
            val jobs = JobRepository(headUnits, connector, Phone, keys, store, {}, scope, shareDir = File(work, "share"))
            val source = FileCatalogueSource(File(sampleRoot, "v1"), File(sampleRoot, "packages"))
            val setup = AutostartSetup(jobs, DefaultCatalogueRepository(source), store, PackageDownloader(source, store, scope), InMemoryPrereleaseRepository(), scope)
            val choice = InMemorySetupRepository()
            val carScreen = CarScreen(socket).also { screen = it }
            val wizard = mainDispatcherRule.track(SetupViewModel(SavedStateHandle(), headUnits, jobs, setup, FakePhoneBluetooth(), choice, connector, scope))

            assertEquals(SetupStep.Welcome, wizard.await("the first page") { true }.step)
            wizard.answer(true)
            wizard.await("pick-the-car") { it.step == SetupStep.PickCar }

            // "Use a network address": the dialog saves the head unit, and the set-up goes on with it.
            headUnits.add(HeadUnit("bench", "Test bench", listOf(TcpEndpoint("127.0.0.1", port))))
            wizard.addedByAddress("bench")

            // The agent answers and knows no such phone: the Pair page, with the box, since this agent has the switch.
            val pairing = wizard.await("the Pair page") { it.step is SetupStep.Pair }
            assertEquals(SetupStep.Pair(stale = false, offersAutostart = true), pairing.step)
            assertEquals(4, pairing.stepCount)
            assertTrue(pairing.enableAutostart)

            wizard.setCode(carScreen.pairingCode())
            wizard.pair()

            // Paired: the car is set up to start the agent, and the page stays for it.
            val ended = wizard.await("the set-up for starting with the car to end") { it.setup?.ended == true }
            assertEquals(SetupStep.StartWithCar, ended.step)
            assertEquals(AutostartStep.Done(changed = true), ended.setup?.step)
            assertEquals(
                listOf(AutostartStage.CHECKING, AutostartStage.GETTING_PLUGIN, AutostartStage.INSTALLING_PLUGIN, AutostartStage.SWITCHING_ON),
                ended.setup?.stages,
            )
            assertEquals(listOf(Phone.id()), carScreen.phoneIds())
            assertEquals(setOf(requireNotNull(headUnits.headUnit("bench").first()?.headUnitId)), keys.headUnitIds)
            val connected = jobs.connection.value as ConnectionState.Connected
            assertEquals(listOf("org.nbtapp.plugin"), connected.installed.map { it.id })
            assertEquals(Autostart(on = true, available = true), connected.autostart)
            assertTrue("the agent's folder has the switch", File(root, "agent/autostart").isFile)

            // iDrive's restart takes the link down, and the set-up is done whatever becomes of it.
            wizard.restartIdrive()
            wizard.await("Done") { it.step == SetupStep.Done }

            assertEquals(listOf("bench"), headUnits.headUnits.first().map { it.id })
            assertFalse("a set-up that was finished is not one that was skipped", choice.skipped.first())
        }
    }
}

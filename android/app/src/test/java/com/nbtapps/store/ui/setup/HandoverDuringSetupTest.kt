// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.lifecycle.SavedStateHandle
import com.nbtapps.store.AgentVectors
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
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.FakePhones
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.ui.pairing.CodeEntry
import com.nbtapps.store.ui.pairrequest.ElapsedClock
import com.nbtapps.store.ui.pairrequest.PairRequest
import com.nbtapps.store.ui.pairrequest.PairRequestStep
import com.nbtapps.store.ui.pairrequest.PairRequestViewModel
import com.nbtapps.store.withTestTimeout
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.job
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * A code another app handed over that pairs a car while the set-up for a new user is open: the real screens' view
 * models over the same head units, job repository and set-up to start with the car, against a stand-in agent.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class HandoverDuringSetupTest {

    /** As in [SetupViewModelTest]: the main dispatcher runs at once and waits in real time. */
    @get:Rule(order = 1)
    val mainDispatcherRule = MainDispatcherRule(main = Dispatchers.Unconfined)

    @get:Rule
    val folder = TemporaryFolder()

    /** Lets each connect through, or holds it until [gate] is completed. */
    private class Gated(private val inner: LinkConnector) : LinkConnector {
        @Volatile
        var gate: CompletableDeferred<Unit>? = null

        override fun supports(endpoint: Endpoint) = inner.supports(endpoint)

        override suspend fun connect(endpoint: Endpoint): Link {
            gate?.await()
            return inner.connect(endpoint)
        }
    }

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = mainDispatcherRule.track(FakeConnector(serial = true))
    private val gated = Gated(connector)
    private val keys = MemoryPairingKeys()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy {
        JobRepository(headUnits, gated, identity, keys, store, {}, scope, shareDir = folder.newFolder("share"))
    }
    private val setup by lazy {
        val source = FileCatalogueSource(null, AgentVectors.file("packages/good"))
        AutostartSetup(jobs, DefaultCatalogueRepository(source), store, PackageDownloader(source, store, scope), InMemoryPrereleaseRepository(), scope)
    }
    private val car = PairedDevice("BMW 12345", "AA:BB:CC:DD:EE:FF", car = true)
    private val phone = FakePhoneBluetooth(devices = listOf(car, PairedDevice("Headphones", "11:22:33:44:55:66")))
    private val choice = InMemorySetupRepository()
    private val phones = FakePhones().also { connector.phones = it }
    private val clock = ElapsedClock { 7_000_000L }

    /** An agent with the switch, and the iDrive plugin there to do the starting. */
    private fun switchable(agent: FakeAgent) {
        agent.capabilities = """["autostart"]"""
        agent.handlers["autostart.get"] = { """{"on":false,"available":true,"starts":0}""" }
        agent.handlers["autostart.set"] = { params ->
            """{"on":${params.getValue("on").jsonPrimitive.content},"available":true,"starts":0}"""
        }
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTestTimeout(60_000) { block() }
    }

    private fun wizard(furtherCar: Boolean = false) =
        mainDispatcherRule.track(
            SetupViewModel(SavedStateHandle(mapOf(SetupViewModel.FURTHER_CAR to furtherCar)), headUnits, jobs, setup, phone, choice, gated, scope),
        )

    private fun handover() = mainDispatcherRule.track(PairRequestViewModel(SavedStateHandle(), headUnits, jobs, setup, phone, gated, scope, clock))

    private suspend fun <T> await(flow: Flow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) }
            ?: throw AssertionError("never came: $what; the link was ${jobs.connection.value}; last was ${(flow as? StateFlow<*>)?.value}")

    /** Every page [wizard] shows from now on. Watched on [scope], so the test does not wait for the watch to end. */
    private fun pages(wizard: SetupViewModel): List<SetupStep> {
        val seen = CopyOnWriteArrayList<SetupStep>()
        scope.launch(Dispatchers.Unconfined) { wizard.uiState.collect { seen += it.step } }
        return seen
    }

    /** The hand-over's whole path: the code comes, [car] is picked from the paired devices, and Pair is tapped. */
    private suspend fun handOver(): PairRequestViewModel {
        val handover = handover()
        handover.handle(PairRequest("420042", 90_000))
        await(handover.uiState, "the prompt") { it.loaded && it.step == PairRequestStep.Prompt }
        handover.pickDevice(car)
        await(handover.uiState, "the pick") { it.device != null }
        handover.pair()
        await(handover.uiState, "paired") { it.step is PairRequestStep.Paired }
        return handover
    }

    private fun issued(): Int = connector.agents.sumOf { it.requestsOf("pairing.issue").size }

    /** What every test asserts once the hand-over has paired: one saved car, the wizard's, and one code, the hand-over's. */
    private suspend fun assertOneCarAndNoCodeAsked(wizard: SetupUiState) {
        val saved = headUnits.headUnits.first()
        assertEquals(1, saved.size)
        assertEquals(saved.single().id, wizard.headUnitId)
        assertEquals("BMW 12345", wizard.name)
        assertEquals("the wizard sent no code", CodeEntry(), wizard.code)
        assertEquals("one code, the hand-over's", 1, issued())
        assertTrue(jobs.connection.value is ConnectionState.Connected)
    }

    @Test
    fun `on Welcome the wizard takes the car paired from outside, and shows it being set up to start the agent`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val wizard = wizard()
        await(wizard.uiState, "Welcome") { it.step == SetupStep.Welcome }
        val seen = pages(wizard)

        handOver()

        val starting = await(wizard.uiState, "start with the car") { it.step == SetupStep.StartWithCar }
        assertOneCarAndNoCodeAsked(starting)
        assertEquals(starting.headUnitId, starting.setup?.headUnitId)
        assertFalse("no code was asked for", seen.any { it is SetupStep.Pair })

        // Moving on is the user's, as on the straight path.
        wizard.moveOn()
        await(wizard.uiState, "Done") { it.step == SetupStep.Done }
    }

    @Test
    fun `on pick-the-car the wizard takes the car paired from outside, with the box cleared there going on to Done`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val wizard = wizard()
        wizard.answer(true)
        await(wizard.uiState, "pick the car") { it.step == SetupStep.PickCar }
        wizard.select(car)
        val seen = pages(wizard)

        val handover = handover()
        handover.handle(PairRequest("420042", 90_000))
        await(handover.uiState, "the prompt") { it.loaded && it.step == PairRequestStep.Prompt }
        handover.setEnableAutostart(false)
        handover.pickDevice(car)
        await(handover.uiState, "the pick") { it.device != null }
        handover.pair()
        await(handover.uiState, "paired") { it.step is PairRequestStep.Paired }

        val done = await(wizard.uiState, "Done") { it.step == SetupStep.Done }
        assertOneCarAndNoCodeAsked(done)
        assertNull("nothing set up", done.setup)
        assertFalse("no code was asked for", seen.any { it is SetupStep.Pair })
    }

    @Test
    fun `mid-connect to the same car the wizard goes on from the hand-over's pairing, and keeps one entry`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val gate = CompletableDeferred<Unit>().also { gated.gate = it }
        val wizard = wizard()
        wizard.answer(true)
        await(wizard.uiState, "pick the car") { it.step == SetupStep.PickCar }
        wizard.select(car)
        wizard.pickCar()
        val id = requireNotNull(await(wizard.uiState, "connecting") { it.step == SetupStep.Connect() }.headUnitId)

        // The hand-over's Pair waits behind the wizard's connect, which then lets both through.
        val handing = launch { handOver() }
        await(headUnits.headUnits, "still one entry") { it.size == 1 }
        delay(200)
        gated.gate = null
        gate.complete(Unit)
        handing.join()

        val starting = await(wizard.uiState, "start with the car") { it.step == SetupStep.StartWithCar }
        assertEquals(id, starting.headUnitId)
        assertOneCarAndNoCodeAsked(starting)
    }

    @Test
    fun `the set-up for a further car is not moved by a car paired from outside`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val wizard = wizard(furtherCar = true)
        await(wizard.uiState, "pick the car") { it.step == SetupStep.PickCar }
        val seen = pages(wizard)

        handOver()
        await(jobs.connection, "the link up") { it is ConnectionState.Connected }
        delay(300)

        // Watched all along, so the state is the latest.
        val state = wizard.uiState.value
        assertEquals(SetupStep.PickCar, state.step)
        assertNull(state.headUnitId)
        assertTrue(seen.all { it == SetupStep.PickCar })
    }
}

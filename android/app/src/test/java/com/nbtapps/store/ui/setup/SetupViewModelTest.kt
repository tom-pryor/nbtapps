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
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.FakePhones
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.IncomingLink
import com.nbtapps.store.data.link.KnockFallback
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.saveCar
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.withTestTimeout
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
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

/** The set-up for a new user, against a stand-in agent over the app's real job repository and set-up. */
@OptIn(ExperimentalCoroutinesApi::class)
class SetupViewModelTest {

    /**
     * The view model's own connect runs on the main dispatcher, and a connect over Bluetooth serial waits between
     * its tries: here the main dispatcher runs at once and waits in real time, as the app's does.
     */
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

        // A connect that only listens for the car's link, as one to a car whose agent leaves the phone's own links.
        override suspend fun connect(endpoint: Endpoint, dials: Boolean, listenOnly: Boolean): Link {
            gate?.await()
            return inner.connect(endpoint, dials, listenOnly)
        }

        // The same, with a probing knock first, for a car whose agent is not known to knock.
        override suspend fun connect(endpoint: Endpoint, dials: Boolean, listenOnly: Boolean, probe: KnockFallback?, dialup: Boolean): Link {
            gate?.await()
            return inner.connect(endpoint, dials, listenOnly, probe, dialup)
        }

        // A knock, as a connect to an agent that dials only when knocked makes: no link of the phone's own.
        override suspend fun knock(endpoint: Endpoint, bursts: Int?, fallback: KnockFallback, dialup: Boolean): Link {
            gate?.await()
            return inner.knock(endpoint, bursts, fallback, dialup)
        }

        // A link the car opens is never held.
        override fun accepts(endpoint: Endpoint) = inner.accepts(endpoint)

        override suspend fun accept(endpoints: Collection<Endpoint>): IncomingLink = inner.accept(endpoints)
    }

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = mainDispatcherRule.track(FakeConnector(serial = true))
    private val gated = Gated(connector)
    private val keys = MemoryPairingKeys()
    private val phoneId = "5f2c9a1e07b34d6688a0c1d2e3f40516"
    private val agentId = "000102030405060708090a0b0c0d0e0f"
    private val identity = object : PhoneIdentity {
        override suspend fun id() = phoneId
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

    /** Agents that ask for `auth`, as a car's does, and what they know of phones. */
    private val phones = FakePhones().also { connector.phones = it }

    /** Whether the stand-in car starts the agent with it. */
    private val on = AtomicBoolean(false)

    /** An agent with the switch, and the iDrive plugin there to do the starting. */
    private fun switchable(agent: FakeAgent) {
        agent.capabilities = """["autostart"]"""
        agent.handlers["autostart.get"] = { """{"on":${on.get()},"available":true,"starts":0}""" }
        agent.handlers["autostart.set"] = { params ->
            on.set(params.getValue("on").jsonPrimitive.content == "true")
            """{"on":${on.get()},"available":true,"starts":0}"""
        }
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTestTimeout(60_000) { block() }
    }

    private fun viewModel(state: SavedStateHandle = SavedStateHandle(), tcp: LinkConnector = connector) =
        mainDispatcherRule.track(SetupViewModel(state, headUnits, jobs, setup, phone, choice, tcp, scope))

    private suspend fun <T> await(flow: Flow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; the link was ${jobs.connection.value}; last was ${(flow as? kotlinx.coroutines.flow.StateFlow<*>)?.value}")

    private suspend fun SetupViewModel.awaitStep(what: String, predicate: (SetupStep) -> Boolean): SetupUiState =
        await(uiState, what) { predicate(it.step) }

    private suspend fun SetupViewModel.awaitPage(step: SetupStep): SetupUiState = awaitStep("$step") { it == step }

    private suspend fun saved(): List<HeadUnit> = headUnits.headUnits.first()

    private fun sets(): Int = connector.agents.sumOf { it.requestsOf("autostart.set").size }

    /** From Welcome to the car picked and Connect tapped. */
    private suspend fun SetupViewModel.pickTheCar() {
        answer(true)
        awaitPage(SetupStep.PickCar)
        select(car)
        pickCar()
    }

    @Test
    fun `on the straight path the car is picked, connected to, paired and set up to start the agent, then Done`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val viewModel = viewModel()
        assertEquals(SetupStep.Welcome, viewModel.awaitPage(SetupStep.Welcome).step)

        viewModel.pickTheCar()

        val pair = viewModel.awaitStep("pair") { it is SetupStep.Pair }
        assertEquals(SetupStep.Pair(stale = false, offersAutostart = true), pair.step)
        assertEquals("BMW 12345", pair.name)
        assertEquals(4, pair.stepCount)
        val unit = saved().single()
        assertEquals(listOf(SerialEndpoint(car.address, car.name)), unit.endpoints)
        assertEquals(unit.id, pair.headUnitId)

        viewModel.setCode("420042")
        viewModel.pair()

        val starting = viewModel.awaitPage(SetupStep.StartWithCar)
        assertEquals(unit.id, starting.setup?.headUnitId)
        val ended = await(viewModel.uiState, "the set-up done") { it.setup?.step is AutostartStep.Done }
        assertEquals(AutostartStep.Done(changed = true), ended.setup?.step)
        assertEquals("the page waits for the user", SetupStep.StartWithCar, ended.step)
        assertTrue(on.get())

        viewModel.moveOn()

        val done = viewModel.awaitPage(SetupStep.Done)
        assertTrue(done.startsWithCar)
        assertEquals(1, connector.agents.size)
        assertFalse("finishing is not skipping", choice.skipped.first())
    }

    @Test
    fun `a connect that fails says why, and Try again connects again`() = test {
        phones.code = "420042"
        connector.unreachable = "No route to the car"
        val viewModel = viewModel()
        viewModel.pickTheCar()

        val failed = viewModel.awaitStep("failed") { it is SetupStep.Connect && it.failure != null }
        assertEquals(SetupStep.Connect("No route to the car"), failed.step)

        connector.unreachable = null
        viewModel.retry()

        viewModel.awaitStep("pair") { it is SetupStep.Pair }
        assertEquals(1, saved().size)
        // Both the user's: their probing knocks may go through the car's Serial Port service once in each burst.
        assertEquals(listOf(KnockFallback.ONCE_PER_BURST, KnockFallback.ONCE_PER_BURST), connector.probes)
    }

    @Test
    fun `a set-up that goes on after its process was stopped connects as the app's own`() = test {
        connector.unreachable = "No route to the car"
        val state = SavedStateHandle()
        val viewModel = viewModel(state)
        viewModel.pickTheCar()
        viewModel.awaitStep("failed") { it is SetupStep.Connect && it.failure != null }
        jobs.disconnect()
        await(jobs.connection, "the end") { it == ConnectionState.Disconnected }

        viewModel(SavedStateHandle(state.keys().associateWith { state.get<Any?>(it) }))

        await(jobs.connection, "the resumed connect's failure") { it is ConnectionState.Failed }
        assertEquals(listOf(KnockFallback.ONCE_PER_BURST, KnockFallback.ONCE_PER_KNOCK), connector.probes)
    }

    @Test
    fun `a connect that only waited for the car's link offers to connect from the phone, which goes on to Pair`() = test {
        phones.code = "420042"
        // A car whose agent dials and leaves the phone's own links: the app's connect only listens for the car.
        val id = headUnits.saveCar(car)
        headUnits.update(id) { it.copy(serialDial = true, phoneOpenedLeft = true) }
        connector.listenOnlyMs = 300
        val viewModel = viewModel()
        viewModel.pickTheCar()

        val failed = viewModel.awaitStep("failed") { it is SetupStep.Connect && it.failure != null }
        assertEquals(SetupStep.Connect("The car didn't connect to this app", fromPhone = true), failed.step)

        viewModel.connectFromPhone()

        viewModel.awaitStep("pair") { it is SetupStep.Pair }
        assertEquals(listOf(true, false), connector.listenOnly)
        assertEquals(1, connector.agents.size)
    }

    @Test
    fun `a failed connect, then the car opening the link itself, goes on to Pair, and the diagnostics name the car`() = test {
        phones.code = "420042"
        connector.unreachable = "the car didn't take the link"
        val viewModel = viewModel()
        viewModel.pickTheCar()
        viewModel.awaitStep("failed") { it is SetupStep.Connect && it.failure != null }
        val copied = viewModel.diagnostics()
        assertTrue(copied, copied.contains("Head unit: BMW 12345"))
        assertFalse(copied, copied.contains(car.address))

        // The pairing window opened on the car has the car dial the phone, which the app takes while it is open.
        withTimeoutOrNull(10_000) { while (connector.dialIn(car.address) == null) delay(10) } ?: throw AssertionError("the app never listened")

        viewModel.awaitStep("pair") { it is SetupStep.Pair }
        assertEquals(1, saved().size)
    }

    @Test
    fun `cancel gives the connect up and removes the car it added, but never one that was saved before`() = test {
        val gate = CompletableDeferred<Unit>().also { gated.gate = it }
        val viewModel = viewModel()
        viewModel.pickTheCar()
        viewModel.awaitPage(SetupStep.Connect())
        assertEquals(1, saved().size)

        viewModel.cancelConnect()

        assertEquals(SetupStep.PickCar, viewModel.awaitPage(SetupStep.PickCar).step)
        await(headUnits.headUnits, "the car removed") { it.isEmpty() }
        await(jobs.connection, "the connect given up") { it == ConnectionState.Disconnected }

        // A car saved before is the user's: it stays.
        val before = headUnits.saveCar(car)
        viewModel.pickCar()
        assertEquals(before, viewModel.awaitPage(SetupStep.Connect()).headUnitId)
        viewModel.cancelConnect()
        viewModel.awaitPage(SetupStep.PickCar)
        delay(200)
        assertEquals(listOf(before), saved().map { it.id })
        gate.complete(Unit)
    }

    @Test
    fun `pick another car after a failed connect removes the car it added, unless an agent answered on it`() = test {
        connector.unreachable = "No route to the car"
        val viewModel = viewModel()
        viewModel.pickTheCar()
        viewModel.awaitStep("failed") { it is SetupStep.Connect && it.failure != null }

        viewModel.pickAnother()

        viewModel.awaitPage(SetupStep.PickCar)
        await(headUnits.headUnits, "the car removed") { it.isEmpty() }

        // One the agent answered on is kept: back from Pair keeps the entry too, and ends the link.
        connector.unreachable = null
        viewModel.pickCar()
        viewModel.awaitStep("pair") { it is SetupStep.Pair }
        assertTrue(viewModel.back().not())
        val picking = viewModel.awaitPage(SetupStep.PickCar)
        assertNull(picking.headUnitId)
        await(jobs.connection, "the link ended") { it == ConnectionState.Disconnected }
        assertEquals(1, saved().size)
    }

    @Test
    fun `a phone the agent takes already skips Pair`() = test {
        keys.put(agentId, phones.pair(phoneId))
        val viewModel = viewModel()
        viewModel.pickTheCar()

        val done = viewModel.awaitPage(SetupStep.Done)
        assertNull("nothing was set up", done.setup)
        assertEquals(0, phones.issued.get())
        assertEquals(4, done.stepCount)
    }

    @Test
    fun `a key the car no longer takes shows the Pair page as stale, and the code pairs the phone anew`() = test {
        keys.put(agentId, phones.pair(phoneId))
        phones.forgetAll()
        phones.code = "000042"
        val viewModel = viewModel()
        viewModel.pickTheCar()

        assertEquals(SetupStep.Pair(stale = true, offersAutostart = false), viewModel.awaitStep("pair") { it is SetupStep.Pair }.step)
        viewModel.setCode("000042")
        viewModel.pair()

        viewModel.awaitPage(SetupStep.Done)
        assertEquals(1, phones.issued.get())
    }

    @Test
    fun `with the box cleared there are three steps, and pairing goes straight to Done`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val viewModel = viewModel()
        viewModel.pickTheCar()
        assertEquals(4, viewModel.awaitStep("pair") { it is SetupStep.Pair }.stepCount)

        viewModel.setEnableAutostart(false)
        assertEquals(3, await(viewModel.uiState, "three steps") { !it.enableAutostart }.stepCount)
        viewModel.setCode("420042")
        viewModel.pair()

        val done = viewModel.awaitPage(SetupStep.Done)
        assertNull(done.setup)
        assertNull(setup.state.value)
        assertEquals(0, sets())
        assertFalse(done.startsWithCar)
    }

    @Test
    fun `an agent with no autostart makes three steps, ticked box or not`() = test {
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.pickTheCar()

        val pair = viewModel.awaitStep("pair") { it is SetupStep.Pair }
        assertEquals(SetupStep.Pair(stale = false, offersAutostart = false), pair.step)
        assertEquals(3, await(viewModel.uiState, "three steps") { it.stepCount == 3 }.stepCount)
        assertTrue(pair.enableAutostart)

        viewModel.setCode("420042")
        viewModel.pair()
        viewModel.awaitPage(SetupStep.Done)
        assertNull(setup.state.value)
    }

    @Test
    fun `a set-up that ends stays shown until the user moves on, or until iDrive restarts`() = test {
        phones.code = "420042"
        connector.onConnect = { agent ->
            switchable(agent)
            agent.restartNeeded = RestartKind.HMI
            agent.handlers["system.restart"] = { "{}" }
        }
        val viewModel = viewModel()
        viewModel.pickTheCar()
        viewModel.awaitStep("pair") { it is SetupStep.Pair }
        viewModel.setCode("420042")
        viewModel.pair()

        val ended = await(viewModel.uiState, "the set-up done") { it.setup?.step is AutostartStep.Done }
        assertEquals(SetupStep.StartWithCar, ended.step)
        assertTrue(ended.restartNeeded)

        viewModel.restartIdrive()

        val done = viewModel.awaitPage(SetupStep.Done)
        assertEquals(AutostartStep.Restarting, done.setup?.step)
    }

    @Test
    fun `once paired the page stays where it is when the link drops`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val state = SavedStateHandle()
        val viewModel = viewModel(state)
        viewModel.pickTheCar()
        viewModel.awaitStep("pair") { it is SetupStep.Pair }
        viewModel.setCode("420042")
        viewModel.pair()
        await(viewModel.uiState, "the set-up done") { it.setup?.step is AutostartStep.Done }

        jobs.disconnect()
        connector.unreachable = "No route to the car"

        delay(200)
        assertEquals(SetupStep.StartWithCar, viewModel.uiState.value.step)
        // And a process restored from the saved state does not connect again.
        val restored = viewModel(SavedStateHandle(state.keys().associateWith { state.get<Any?>(it) }))
        assertEquals(SetupStep.StartWithCar, restored.awaitPage(SetupStep.StartWithCar).step)
        assertEquals(1, connector.agents.size)

        viewModel.moveOn()
        viewModel.awaitPage(SetupStep.Done)
        assertEquals(ConnectionState.Disconnected, jobs.connection.value)
    }

    @Test
    fun `a car saved and paired from outside while the car is being picked is taken as the car, and used as it is`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val viewModel = viewModel()
        viewModel.answer(true)
        viewModel.awaitPage(SetupStep.PickCar)

        // Another app's code: the car is saved, connected to and paired, and set up to start the agent.
        val id = headUnits.saveCar(car)
        assertTrue(jobs.connect(id).isFailure)
        assertTrue(jobs.pair("420042").isSuccess)
        setup.begin(id)

        // Picking the car now changes nothing: it is the car already.
        viewModel.select(car)
        viewModel.pickCar()

        val starting = viewModel.awaitPage(SetupStep.StartWithCar)
        assertEquals(id, starting.headUnitId)
        assertEquals("no second link", 1, connector.agents.size)
        assertEquals(listOf(id), saved().map { it.id })
    }

    @Test
    fun `the same car paired from outside while the set-up connects goes on from there`() = test {
        phones.code = "420042"
        val gate = CompletableDeferred<Unit>().also { gated.gate = it }
        val viewModel = viewModel()
        viewModel.pickTheCar()
        val connecting = viewModel.awaitPage(SetupStep.Connect())
        val id = requireNotNull(connecting.headUnitId)

        // Another app's code for the same car: it is saved already, and its connect waits behind this one.
        assertEquals(id, headUnits.saveCar(car))
        val outside = scope.launch { if (jobs.connect(id).isFailure) jobs.pair("420042") }
        gated.gate = null
        gate.complete(Unit)

        viewModel.awaitPage(SetupStep.Done)
        outside.join()
        assertTrue(jobs.connection.value is ConnectionState.Connected)

        // Given up now, it is not removed: an agent answered on it.
        assertEquals(listOf(id), saved().map { it.id })
    }

    @Test
    fun `a failed connect, then the car paired from outside, goes on to Done and keeps the car`() = test {
        phones.code = "420042"
        connector.unreachable = "No route to the car"
        val viewModel = viewModel()
        viewModel.pickTheCar()
        val failed = viewModel.awaitStep("failed") { it is SetupStep.Connect && it.failure != null }
        val id = requireNotNull(failed.headUnitId)

        connector.unreachable = null
        assertTrue(jobs.connect(id).isFailure)
        assertTrue(viewModel.awaitStep("pair") { it is SetupStep.Pair }.step is SetupStep.Pair)
        assertTrue(jobs.pair("420042").isSuccess)

        viewModel.awaitPage(SetupStep.Done)
        viewModel.pickAnother()
        delay(200)
        assertEquals(listOf(id), saved().map { it.id })
    }

    @Test
    fun `a further car begins at picking it, and back there leaves the set-up`() = test {
        val viewModel = viewModel(SavedStateHandle(mapOf(SetupViewModel.FURTHER_CAR to true)))
        assertTrue(viewModel.furtherCar)
        assertEquals(SetupStep.PickCar, viewModel.awaitPage(SetupStep.PickCar).step)
        assertTrue(viewModel.back())

        // From Welcome, back from picking the car asks the question again, and back there skips the set-up.
        val first = viewModel()
        first.answer(true)
        first.awaitPage(SetupStep.PickCar)
        assertFalse(first.back())
        first.awaitPage(SetupStep.Welcome)
        assertTrue(first.back())
        assertTrue(await(choice.skipped, "skipped") { it })
    }

    @Test
    fun `a head unit added by network address is the one connected to`() = test {
        val tcp = mainDispatcherRule.track(FakeConnector())
        assertFalse(viewModel().uiState.value.takesAddress)
        val viewModel = viewModel(tcp = tcp)
        assertTrue(viewModel.uiState.value.takesAddress)
        viewModel.answer(true)
        viewModel.awaitPage(SetupStep.PickCar)

        viewModel.beginAddress()
        delay(100)
        // The add dialog saves it, and hands its id over.
        val bench = HeadUnit("bench", "Bench", listOf(TcpEndpoint("192.168.0.1", 7144)))
        headUnits.add(bench)
        viewModel.addedByAddress("bench")

        // The serial connector has no way to it: the connect fails, and the page says so.
        val failed = viewModel.awaitStep("failed") { it is SetupStep.Connect && it.failure != null }
        assertEquals("bench", failed.headUnitId)
        assertNull("no Bluetooth endpoint", failed.carInReach)
        viewModel.pickAnother()
        await(headUnits.headUnits, "the bench removed") { it.isEmpty() }
    }

    @Test
    fun `whether the car is in reach follows the phone's Bluetooth connections`() = test {
        connector.unreachable = "No route to the car"
        val viewModel = viewModel()
        viewModel.pickTheCar()
        assertEquals(false, viewModel.awaitStep("failed") { it is SetupStep.Connect && it.failure != null }.carInReach)

        phone.links = setOf(car.address.lowercase())
        viewModel.refreshBluetooth()

        assertEquals(true, await(viewModel.uiState, "in reach") { it.carInReach == true }.carInReach)
    }
}

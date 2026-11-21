// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import androidx.lifecycle.SavedStateHandle
import com.nbtapps.store.AgentVectors
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.FakePhoneBluetooth
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.InMemoryPrereleaseRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.DefaultCatalogueRepository
import com.nbtapps.store.data.catalogue.FileCatalogueSource
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.data.jobs.FakeCheck
import com.nbtapps.store.data.jobs.FakeChecks
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.FakePhones
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.LinkConnectors
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.withTestTimeout
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.job
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The screen for a code another app handed over, against a stand-in agent over the app's real job repository. */
class PairRequestViewModelTest {

    @get:Rule(order = 1)
    val mainDispatcherRule = MainDispatcherRule()

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
    }

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = mainDispatcherRule.track(FakeConnector())

    /** The way to a car picked from the phone's paired devices, as the debug build has both. */
    private val serial = mainDispatcherRule.track(FakeConnector(serial = true))
    private val gated = Gated(LinkConnectors(listOf(connector, serial)))
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

    @Volatile
    private var now = 7_000_000L
    private val clock = ElapsedClock { now }

    private val car = HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144)))
    private val other = HeadUnit("other", "Other car", listOf(TcpEndpoint("192.168.0.2", 7144)))

    /** Agents that ask for `auth`, as a car's does, and what they know of phones. */
    private val phones = FakePhones().also {
        connector.phones = it
        serial.phones = it
    }

    /** A car among the phone's paired devices, and another paired device. */
    private val device = PairedDevice("BMW 12345", "AA:BB:CC:DD:EE:FF", car = true)
    private val phone = FakePhoneBluetooth(devices = listOf(device, PairedDevice("Headphones", "11:22:33:44:55:66")))

    init {
        // A connect a test held at the gate is let through first, should the test have failed before it did.
        mainDispatcherRule.letGo { gated.gate?.complete(Unit) }
    }

    private fun test(saved: List<HeadUnit> = listOf(car), block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTestTimeout(60_000) {
            saved.forEach { headUnits.add(it) }
            block()
        }
    }

    private fun viewModel(state: SavedStateHandle = SavedStateHandle(), links: LinkConnector = gated) =
        mainDispatcherRule.track(PairRequestViewModel(state, headUnits, jobs, setup, phone, links, scope, clock))

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private suspend fun PairRequestViewModel.awaitStep(what: String, predicate: (PairRequestStep) -> Boolean): PairRequestUiState =
        await(uiState, what) { it.loaded && predicate(it.step) }

    private fun issued(): List<String> =
        connector.agents.flatMap { agent -> agent.requestsOf("pairing.issue").map { it.getValue("code").jsonPrimitive.content } }

    @Test
    fun `one tap of Pair connects to the only car and sends the code, and the phone is paired`() = test {
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 90_000))

        val prompt = viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        assertEquals("420042", prompt.code)
        assertEquals(car, prompt.chosen)
        assertEquals(90_000L, prompt.remainingMs)
        assertTrue(prompt.canPair)
        assertTrue("nothing is connected before the tap", connector.agents.isEmpty())

        viewModel.pair()

        assertEquals(PairRequestStep.Paired("Test car"), viewModel.awaitStep("paired") { it is PairRequestStep.Paired }.step)
        assertEquals("car", (jobs.connection.value as ConnectionState.Connected).headUnitId)
        assertArrayEquals(phones.keys[phoneId], keys.key(agentId))
        assertEquals(listOf("420042"), issued())
    }

    @Test
    fun `a phone the agent takes already is paired already, and nothing is sent`() = test {
        keys.put(agentId, phones.pair(phoneId))
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))

        viewModel.pair()

        assertEquals(PairRequestStep.AlreadyPaired("Test car"), viewModel.awaitStep("already paired") { it is PairRequestStep.AlreadyPaired }.step)
        assertEquals(emptyList<String>(), issued())
        assertEquals(0, phones.issued.get())
    }

    @Test
    fun `a link that is up to the car already is used as it is`() = test {
        keys.put(agentId, phones.pair(phoneId))
        assertTrue(jobs.connect("car").isSuccess)
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))

        viewModel.pair()

        assertEquals(PairRequestStep.AlreadyPaired("Test car"), viewModel.awaitStep("already paired") { it is PairRequestStep.AlreadyPaired }.step)
        assertEquals("no second link", 1, connector.agents.size)
    }

    @Test
    fun `a link that waits for a code takes this one, without connecting again`() = test {
        phones.code = "420042"
        assertTrue(jobs.connect("car").isFailure)
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))

        viewModel.pair()

        viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertEquals(1, connector.agents.size)
    }

    @Test
    fun `a key the agent no longer takes is dropped, and the code pairs the phone anew`() = test {
        keys.put(agentId, phones.pair(phoneId))
        // The car's phones were forgotten, and the other app got a new code from it.
        phones.forgetAll()
        phones.code = "000042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("000042", 60_000))

        viewModel.pair()

        viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertEquals(2, connector.agents.size)
        assertArrayEquals(phones.keys[phoneId], keys.key(agentId))
    }

    @Test
    fun `a wrong code is refused in this screen's words, and the link goes on waiting`() = test {
        phones.code = "111111"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("222222", null))

        viewModel.pair()

        val refused = viewModel.awaitStep("refused") { it is PairRequestStep.Refused }
        assertEquals(PairRequestStep.Refused(pairRequestRefusal("rejected", "Test car")), refused.step)
        assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)
        assertFalse("nothing to tap Pair for", refused.canPair)
        assertFalse(refused.canRetry)
    }

    @Test
    fun `a code for a car with no pairing window open is refused`() = test {
        phones.code = null
        val viewModel = viewModel()
        viewModel.handle(PairRequest("222222", null))

        viewModel.pair()

        val refused = viewModel.awaitStep("refused") { it is PairRequestStep.Refused }.step as PairRequestStep.Refused
        assertEquals(pairRequestRefusal("forbidden", "Test car"), refused.message)
        assertTrue(refused.message.startsWith("Test car isn't waiting for a pairing code"))
    }

    @Test
    fun `the refusals say nothing of the car's screen`() {
        for (code in listOf("forbidden", "rejected", "unsupported", "bad-request", "busy")) {
            val text = pairRequestRefusal(code, "Test car")
            assertFalse(text, text.contains("screen"))
        }
    }

    @Test
    fun `a code that has run out is not sent, and nothing is connected for it`() = test {
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 30_000))
        viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }

        now += 30_000
        viewModel.pair()

        assertEquals(PairRequestStep.TimeUp, viewModel.awaitStep("time up") { it == PairRequestStep.TimeUp }.step)
        assertTrue(connector.agents.isEmpty())
    }

    @Test
    fun `a code that runs out while the link is opened is not sent over it`() = test {
        phones.code = "420042"
        connector.onConnect = { now += 31_000 }
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 30_000))

        viewModel.pair()

        viewModel.awaitStep("time up") { it == PairRequestStep.TimeUp }
        assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)
        assertEquals(emptyList<String>(), issued())
    }

    @Test
    fun `a code with no time left is shown as run out at once`() = test {
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 0))

        val state = viewModel.awaitStep("time up") { it == PairRequestStep.TimeUp }
        assertEquals(0L, state.remainingMs)
        assertFalse(state.canPair)
    }

    @Test
    fun `with no link to the car the reason is shown, and Retry tries again`() = test {
        phones.code = "420042"
        connector.unreachable = "no route to the bench"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 60_000))

        viewModel.pair()

        val failed = viewModel.awaitStep("no link") { it is PairRequestStep.NoLink }
        assertEquals(PairRequestStep.NoLink("no route to the bench"), failed.step)
        assertTrue(failed.canRetry)

        connector.unreachable = null
        viewModel.pair()

        viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertEquals(listOf("420042"), issued())
    }

    @Test
    fun `a connect that only waited for the car's link offers to connect from the phone, which pairs the phone`() = test(
        saved = listOf(HeadUnit("bt", "Test car", listOf(SerialEndpoint(device.address, device.name)), serialDial = true, phoneOpenedLeft = true)),
    ) {
        phones.code = "420042"
        // The view model's connect runs on the test's main dispatcher, whose time stands still: no wait at all.
        serial.listenOnlyMs = 0
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 60_000))

        viewModel.pair()

        val failed = viewModel.awaitStep("no link") { it is PairRequestStep.NoLink }
        assertEquals(PairRequestStep.NoLink("The car didn't connect to this app", fromPhone = true), failed.step)
        assertTrue(failed.canRetry)

        viewModel.connectFromPhone()

        viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertEquals(listOf(true, false), serial.listenOnly)
        assertEquals(listOf("420042"), serialIssued())
    }

    @Test
    fun `Retry once the code has run out connects nothing`() = test {
        connector.unreachable = "no route to the bench"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 60_000))
        viewModel.pair()
        viewModel.awaitStep("no link") { it is PairRequestStep.NoLink }

        connector.unreachable = null
        now += 60_000
        viewModel.pair()

        viewModel.awaitStep("time up") { it == PairRequestStep.TimeUp }
        assertTrue(connector.agents.isEmpty())
    }

    @Test
    fun `a new code mid-flow takes the place of the old, and what the old one's connect ends in is dropped`() = test {
        val gate = CompletableDeferred<Unit>().also { gated.gate = it }
        val viewModel = viewModel()
        viewModel.handle(PairRequest("111111", null))
        viewModel.pair()
        viewModel.awaitStep("connecting") { it == PairRequestStep.Connecting }

        viewModel.handle(PairRequest("222222", 60_000))
        val prompt = viewModel.awaitStep("the prompt again") { it == PairRequestStep.Prompt }
        assertEquals("222222", prompt.code)
        assertEquals(60_000L, prompt.remainingMs)

        // The old connect ends with a link that waits for a code, and the old code does not go over it.
        phones.code = "222222"
        gated.gate = null
        gate.complete(Unit)
        await(jobs.connection, "the link waiting") { it is ConnectionState.NeedsPairing }
        assertEquals(PairRequestStep.Prompt, viewModel.uiState.value.step)
        assertEquals(emptyList<String>(), issued())

        viewModel.pair()

        viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertEquals(listOf("222222"), issued())
    }

    @Test
    fun `the same code again changes nothing`() = test {
        phones.code = "111111"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("222222", null))
        viewModel.pair()
        viewModel.awaitStep("refused") { it is PairRequestStep.Refused }

        viewModel.handle(PairRequest("222222", 60_000))

        val state = viewModel.awaitStep("still refused") { true }
        assertTrue(state.step is PairRequestStep.Refused)
        assertNull("the deadline is the first request's", state.remainingMs)
    }

    @Test
    fun `the countdown survives a rotation and a restart of the process`() = test {
        val state = SavedStateHandle()
        val first = viewModel(state)
        first.handle(PairRequest("420042", 90_000))
        first.awaitStep("the prompt") { it == PairRequestStep.Prompt }

        // The activity is made again and hands the same request to the same saved state.
        now += 20_000
        val rotated = viewModel(state)
        rotated.handle(PairRequest("420042", 90_000))
        assertEquals(70_000L, rotated.awaitStep("the prompt") { it == PairRequestStep.Prompt }.remainingMs)

        // The process is stopped: its saved state is all that is left.
        now += 30_000
        val restored = viewModel(SavedStateHandle(state.keys().associateWith { state.get<Any?>(it) }))
        restored.handle(PairRequest("420042", 90_000))
        assertEquals(40_000L, restored.awaitStep("the prompt") { it == PairRequestStep.Prompt }.remainingMs)
    }

    @Test
    fun `of several cars the one the link refers to is chosen, and with none the user picks`() = test(saved = listOf(car, other)) {
        val unlinked = viewModel()
        unlinked.handle(PairRequest("420042", null))
        val none = unlinked.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        assertEquals(listOf(car, other), none.headUnits)
        assertNull(none.chosen)
        assertFalse(none.canPair)
        unlinked.pair()
        assertTrue("nothing to connect to", connector.agents.isEmpty())

        unlinked.choose("other")
        assertEquals(other, await(unlinked.uiState, "the pick") { it.chosen != null }.chosen)

        // The app's last link was to the other car.
        connector.unreachable = "no route to the bench"
        assertTrue(jobs.connect("other").isFailure)
        val linked = viewModel()
        linked.handle(PairRequest("420042", null))
        assertEquals(other, linked.awaitStep("the prompt") { it == PairRequestStep.Prompt }.chosen)

        // What the user picks goes before it.
        linked.choose("car")
        assertEquals(car, await(linked.uiState, "the pick") { it.chosen == car }.chosen)
    }

    // ---- picking the car from the phone's paired devices ----

    private fun serialIssued(): List<String> =
        serial.agents.flatMap { agent -> agent.requestsOf("pairing.issue").map { it.getValue("code").jsonPrimitive.content } }

    @Test
    fun `with nothing saved a device picked is saved once on Pair, and the phone is paired with it`() = test(saved = emptyList()) {
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 90_000))
        val none = viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        assertTrue("nothing else to choose from", none.picking)
        assertNull(none.device)
        assertFalse(none.canPair)

        viewModel.pickDevice(device)
        val picked = await(viewModel.uiState, "the pick") { it.device != null }
        assertEquals(device.address, picked.device?.address)
        assertEquals("BMW 12345", picked.carName)
        assertTrue("it is added on Pair", picked.newCar)
        assertFalse(picked.prePicked)
        assertTrue(picked.canPair)
        assertTrue("nothing saved before Pair", headUnits.headUnits.first().isEmpty())
        assertTrue("nothing connected before Pair", serial.agents.isEmpty())

        viewModel.pair()

        val paired = viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertEquals(PairRequestStep.Paired("BMW 12345"), paired.step)
        val unit = headUnits.headUnits.first().single()
        assertEquals(listOf(SerialEndpoint(device.address, device.name)), unit.endpoints)
        assertEquals(unit, paired.chosen)
        assertFalse(paired.picking)
        assertTrue("this request added it", paired.newCar)
        assertEquals(unit.id, (jobs.connection.value as ConnectionState.Connected).headUnitId)
        assertEquals(listOf("420042"), serialIssued())
    }

    @Test
    fun `a device saved already is the head unit paired with, and is not added again`() = test(saved = emptyList()) {
        val mine = HeadUnit("mine", "My car", listOf(SerialEndpoint(device.address.lowercase(), device.name)))
        headUnits.add(mine)
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))
        val prompt = viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        assertEquals(mine, prompt.chosen)
        assertFalse(prompt.picking)

        viewModel.pickDifferent()
        viewModel.pickDevice(device)
        val picked = await(viewModel.uiState, "the pick") { it.device != null }
        assertTrue(picked.picking)
        assertNull("the saved car is not the one any more", picked.chosen)
        assertFalse("it is saved already", picked.newCar)

        viewModel.pair()

        val paired = viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertEquals(PairRequestStep.Paired("My car"), paired.step)
        assertEquals(listOf("mine"), headUnits.headUnits.first().map { it.id })
        assertEquals("mine", paired.chosen?.id)
        assertFalse(paired.newCar)
    }

    @Test
    fun `a device picked while other cars are saved is added beside them, and paired with`() = test(saved = listOf(car, other)) {
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))
        viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }

        viewModel.pickDifferent()
        val picking = await(viewModel.uiState, "picking") { it.picking }
        assertFalse("no device picked yet", picking.canPair)
        viewModel.pickDevice(device)
        assertTrue(await(viewModel.uiState, "the pick") { it.device != null }.newCar)

        viewModel.pair()

        val paired = viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        val saved = headUnits.headUnits.first()
        assertEquals(3, saved.size)
        val added = saved.single { it.id != car.id && it.id != other.id }
        assertEquals(added, paired.chosen)
        assertTrue(paired.newCar)
        assertTrue(connector.agents.isEmpty())

        // Choosing a saved car puts a device picked aside.
        val again = viewModel()
        again.handle(PairRequest("135790", null))
        again.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        again.pickDifferent()
        again.pickDevice(device)
        again.choose(other.id)
        val chosen = await(again.uiState, "the saved car") { it.chosen == other }
        assertFalse(chosen.picking)
        assertNull(chosen.device)
    }

    @Test
    fun `a code that has run out with a device picked saves nothing`() = test(saved = emptyList()) {
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", 30_000))
        viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        viewModel.pickDevice(device)
        await(viewModel.uiState, "the pick") { it.device != null }

        now += 30_000
        viewModel.pair()

        viewModel.awaitStep("time up") { it == PairRequestStep.TimeUp }
        delay(200)
        assertTrue(headUnits.headUnits.first().isEmpty())
        assertTrue(serial.agents.isEmpty())
    }

    @Test
    fun `a device the picker picked is said to be picked for the user only while it is connected`() = test(saved = emptyList()) {
        phone.links = setOf(device.address)
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))
        viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }

        viewModel.pickDevice(device, byDefault = true)
        assertTrue(await(viewModel.uiState, "picked for the user") { it.prePicked }.prePicked)

        phone.links = emptySet()
        phone.refreshConnected()
        assertFalse(await(viewModel.uiState, "not connected") { it.device != null && !it.prePicked }.prePicked)

        // Picked by the user, it is the user's pick.
        phone.links = setOf(device.address)
        phone.refreshConnected()
        viewModel.pickDevice(device)
        delay(200)
        assertFalse(viewModel.uiState.value.prePicked)
    }

    @Test
    fun `a network address is offered only in a build that reaches one`() = test {
        assertTrue(await(viewModel().uiState, "loaded") { it.loaded }.takesAddress)
        assertFalse(await(viewModel(links = serial).uiState, "loaded") { it.loaded }.takesAddress)
    }

    @Test
    fun `a car added by network address is chosen, and pairing goes on with it without a second tap`() = test(saved = emptyList()) {
        phones.code = "420042"
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))
        val none = viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        assertTrue(none.headUnits.isEmpty())
        assertFalse(none.canPair)

        // The add flow gives the id, and the head unit is saved a moment later.
        viewModel.added("car")
        assertEquals(PairRequestStep.Connecting, viewModel.awaitStep("waiting for it") { true }.step)
        headUnits.add(car)

        val paired = viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertEquals(PairRequestStep.Paired("Test car"), paired.step)
        assertTrue("this request added it", paired.newCar)
    }

    // ---- starting with the car ----

    private fun sets(): Int = connector.agents.sumOf { it.requestsOf("autostart.set").size }

    @Test
    fun `with the box ticked, as it is to begin with, the car is set up to start the agent once the phone is paired`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))
        assertTrue(viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }.enableAutostart)

        viewModel.pair()

        viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        val done = await(viewModel.uiState, "the set-up done") { it.setup is AutostartStep.Done }
        assertEquals(AutostartStep.Done(changed = true), done.setup)
        assertEquals(PairRequestStep.Paired("Test car"), done.step)
        assertEquals("car", setup.state.value?.headUnitId)
        assertTrue(on.get())
    }

    @Test
    fun `with the box cleared, pairing sets nothing up`() = test {
        phones.code = "420042"
        connector.onConnect = ::switchable
        val state = SavedStateHandle()
        val viewModel = viewModel(state)
        viewModel.handle(PairRequest("420042", null))
        viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        viewModel.setEnableAutostart(false)
        // Kept in the saved state, as the code is.
        assertFalse(await(viewModel(state).uiState, "the box cleared") { it.loaded && !it.enableAutostart }.enableAutostart)

        viewModel.pair()

        val paired = viewModel.awaitStep("paired") { it is PairRequestStep.Paired }
        assertFalse(paired.enableAutostart)
        assertNull(paired.setup)
        assertNull(setup.state.value)
        assertEquals(0, sets())
    }

    @Test
    fun `a phone paired already has the car set up as well, over a new link and over the one there is`() = test {
        keys.put(agentId, phones.pair(phoneId))
        connector.onConnect = ::switchable
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))

        viewModel.pair()

        viewModel.awaitStep("already paired") { it is PairRequestStep.AlreadyPaired }
        assertEquals(AutostartStep.Done(changed = true), await(viewModel.uiState, "the set-up done") { it.setup is AutostartStep.Done }.setup)

        // Again, with the link up: the set-up runs again, and finds nothing to change.
        val again = viewModel()
        again.handle(PairRequest("135790", null))
        again.pair()
        again.awaitStep("already paired") { it is PairRequestStep.AlreadyPaired }
        assertEquals(AutostartStep.Done(changed = false), await(again.uiState, "the set-up done") { it.setup == AutostartStep.Done(changed = false) }.setup)
        assertEquals(1, connector.agents.size)
        assertEquals(1, sets())

        // And once more with the box cleared: the set-up from before says nothing of this pairing, and goes.
        val cleared = viewModel()
        cleared.handle(PairRequest("246802", null))
        cleared.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        cleared.setEnableAutostart(false)
        cleared.pair()
        cleared.awaitStep("already paired") { it is PairRequestStep.AlreadyPaired }
        assertNull(setup.state.value)
        assertEquals(1, sets())
    }

    @Test
    fun `a request replaced while its connect was under way sets nothing up when the connect ends`() = test {
        keys.put(agentId, phones.pair(phoneId))
        connector.onConnect = ::switchable
        val gate = CompletableDeferred<Unit>().also { gated.gate = it }
        val viewModel = viewModel()
        viewModel.handle(PairRequest("111111", null))
        viewModel.pair()
        viewModel.awaitStep("connecting") { it == PairRequestStep.Connecting }

        viewModel.handle(PairRequest("222222", null))
        viewModel.awaitStep("the prompt again") { it == PairRequestStep.Prompt }
        gated.gate = null
        gate.complete(Unit)
        await(jobs.connection, "the link up") { it is ConnectionState.Connected }
        // The connect returns a moment after the link is shown up, and its end is dropped then.
        delay(300)

        assertEquals(PairRequestStep.Prompt, viewModel.uiState.value.step)
        assertNull(setup.state.value)
        assertEquals(0, sets())
    }

    @Test
    fun `with the box cleared, what the checks found is pointed to once the phone is paired`() = test {
        keys.put(agentId, phones.pair(phoneId))
        val checks = FakeChecks().apply { add(FakeCheck("aa-fix", listOf("/opt/demo/bin/demod"))) }
        connector.onConnect = { agent ->
            agent.capabilities = """["checks"]"""
            agent.checks = checks
        }
        val viewModel = viewModel()
        viewModel.handle(PairRequest("420042", null))
        viewModel.awaitStep("the prompt") { it == PairRequestStep.Prompt }
        viewModel.setEnableAutostart(false)

        viewModel.pair()

        val line = await(viewModel.uiState, "the line") { it.showsChecks }
        assertEquals(PairRequestStep.AlreadyPaired("Test car"), line.step)
        assertEquals(1, line.openChecks)
        assertNull(line.setup)
    }
}

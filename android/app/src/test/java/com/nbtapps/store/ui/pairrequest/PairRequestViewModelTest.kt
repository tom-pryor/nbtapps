// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import androidx.lifecycle.SavedStateHandle
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.FakePhones
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.TcpEndpoint
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.jsonPrimitive
import org.junit.After
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

    @get:Rule
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
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector()
    private val gated = Gated(connector)
    private val keys = MemoryPairingKeys()
    private val phoneId = "5f2c9a1e07b34d6688a0c1d2e3f40516"
    private val agentId = "000102030405060708090a0b0c0d0e0f"
    private val identity = object : PhoneIdentity {
        override suspend fun id() = phoneId
        override val name = "Pixel 9"
    }
    private val jobs by lazy {
        JobRepository(headUnits, gated, identity, keys, PackageStore(folder.newFolder("packages")), {}, scope, shareDir = folder.newFolder("share"))
    }

    @Volatile
    private var now = 7_000_000L
    private val clock = ElapsedClock { now }

    private val car = HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144)))
    private val other = HeadUnit("other", "Other car", listOf(TcpEndpoint("192.168.0.2", 7144)))

    /** Agents that ask for `auth`, as a car's does, and what they know of phones. */
    private val phones = FakePhones().also { connector.phones = it }

    @After
    fun tearDown() = scope.cancel()

    private fun test(saved: List<HeadUnit> = listOf(car), block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            saved.forEach { headUnits.add(it) }
            block()
        }
    }

    private fun viewModel(state: SavedStateHandle = SavedStateHandle()) = PairRequestViewModel(state, headUnits, jobs, scope, clock)

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

    @Test
    fun `a car added on the screen is chosen, and pairing goes on with it without a second tap`() = test(saved = emptyList()) {
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

        assertEquals(PairRequestStep.Paired("Test car"), viewModel.awaitStep("paired") { it is PairRequestStep.Paired }.step)
    }
}

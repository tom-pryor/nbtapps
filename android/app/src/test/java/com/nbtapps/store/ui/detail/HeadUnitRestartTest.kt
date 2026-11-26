// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.detail

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
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.Refusal
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.ui.restart.RestartStep
import com.nbtapps.store.withTestTimeout
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * The agent's card's Restart iDrive and Reboot on the head unit's page, over the fake agent: Restart iDrive asks first
 * and says a refusal in its question, Reboot is sent at once and says its refusal under the card. The tests wait for
 * what they look for, never for a time.
 */
class HeadUnitRestartTest {

    @get:Rule(order = 1)
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = mainDispatcherRule.track(CoroutineScope(SupervisorJob() + Dispatchers.Default))
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = mainDispatcherRule.track(FakeConnector(serial = true))
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Test phone"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy { JobRepository(headUnits, connector, identity, MemoryPairingKeys(), store, {}, scope, now = { 1234L }) }
    private val setup by lazy {
        val source = FileCatalogueSource(null, AgentVectors.file("packages/good"))
        AutostartSetup(jobs, DefaultCatalogueRepository(source, scope), store, PackageDownloader(source, store, scope), InMemoryPrereleaseRepository(), scope)
    }

    private val car = HeadUnit("car", "Test car", listOf(SerialEndpoint("00:11:22:33:44:55", "Test car")))

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTestTimeout(60_000) {
            headUnits.add(car)
            block()
        }
    }

    /** Every agent from here on answers `system.restart` with [answer], given the request's kind. */
    private fun agents(answer: (kind: String) -> String = { "{}" }) {
        connector.onConnect = { agent -> agent.handlers["system.restart"] = { params -> answer(params.getValue("kind").jsonPrimitive.content) } }
    }

    private fun viewModel() = mainDispatcherRule.track(HeadUnitDetailViewModel("car", headUnits, jobs, connector, FakePhoneBluetooth(), setup, scope))

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private fun kinds() = connector.agent.requestsOf("system.restart").map { it.getValue("kind").jsonPrimitive.content }

    @Test
    fun `Restart iDrive asks first, and only Restart sends it, to this head unit's agent`() = test {
        agents()
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)

        viewModel.askRestart()
        assertEquals(RestartStep.Asking("car", terminalOpen = false, portOpen = false), viewModel.restartQuestion.state.value)
        assertEquals("nothing is sent before Restart", emptyList<String>(), kinds())

        viewModel.restartQuestion.confirm()
        await(viewModel.restartQuestion.state, "the answer") { it == RestartStep.Closed }
        assertEquals(listOf("hmi"), kinds())
    }

    @Test
    fun `Cancel sends nothing`() = test {
        agents()
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)

        viewModel.askRestart()
        viewModel.restartQuestion.cancel()
        assertEquals(RestartStep.Closed, viewModel.restartQuestion.state.value)
        assertEquals(emptyList<String>(), kinds())
    }

    @Test
    fun `a refusal of Restart iDrive is said in its question, not under the card`() = test {
        agents { throw Refusal("busy") }
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)

        viewModel.askRestart()
        viewModel.restartQuestion.confirm()
        val failed = await(viewModel.restartQuestion.state, "the refusal") { it is RestartStep.Failed } as RestartStep.Failed
        assertEquals(AgentText.restartRefusal("busy"), failed.text)
        assertNull(viewModel.uiState.value.restartError)
    }

    @Test
    fun `Restart iDrive asks nothing where the link is to no head unit`() = test {
        val viewModel = viewModel()
        viewModel.askRestart()
        assertEquals(RestartStep.Closed, viewModel.restartQuestion.state.value)
    }

    @Test
    fun `Reboot is sent at once, with no question, and its refusal goes under the card`() = test {
        agents { kind -> if (kind == "reboot") throw Refusal("busy") else "{}" }
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)

        viewModel.reboot()
        assertEquals(RestartStep.Closed, viewModel.restartQuestion.state.value)
        val page = await(viewModel.uiState, "the refusal under the card") { it.restartError != null }
        assertEquals(AgentText.request("busy"), page.restartError)
        assertEquals(listOf("reboot"), kinds())
    }
}

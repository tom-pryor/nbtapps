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
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.SerialResetOutcome
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.SerialEndpoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The head unit's page and its Troubleshooting card: when it is there, and what it says a reset came to. */
class HeadUnitTroubleshootingTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector(serial = true)
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Test phone"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy { JobRepository(headUnits, connector, identity, MemoryPairingKeys(), store, {}, scope, now = { 1234L }) }
    private val setup by lazy {
        val source = FileCatalogueSource(null, AgentVectors.file("packages/good"))
        AutostartSetup(jobs, DefaultCatalogueRepository(source), store, PackageDownloader(source, store, scope), InMemoryPrereleaseRepository(), scope)
    }

    private val car = HeadUnit("car", "Test car", listOf(SerialEndpoint("00:11:22:33:44:55", "Test car")))

    @After
    fun tearDown() = scope.cancel()

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(car)
            block()
        }
    }

    private fun viewModel() = HeadUnitDetailViewModel("car", headUnits, jobs, connector, FakePhoneBluetooth(), setup, scope)

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    @Test
    fun `the card is there while the agent can reset the link, and while a reset from the page has something to say`() {
        assertFalse(showsTroubleshooting(HeadUnitDetailUiState()))
        assertTrue(showsTroubleshooting(HeadUnitDetailUiState(canResetSerial = true)))
        // The reset ends the link it was asked on: the card stays for it meanwhile, and for its outcome.
        assertTrue(showsTroubleshooting(HeadUnitDetailUiState(serialReset = SerialResetUi.Working)))
        assertTrue(showsTroubleshooting(HeadUnitDetailUiState(serialReset = SerialResetUi.Done("The car ended the Bluetooth link."))))
    }

    @Test
    fun `an agent that lists the capability offers the reset, and one that does not, or no link, does not`() = test {
        connector.onConnect = { it.capabilities = """["serial-reset"]""" }
        val viewModel = viewModel()
        assertFalse(await(viewModel.uiState, "the page") { it.loaded }.canResetSerial)

        assertTrue(jobs.connect("car").isSuccess)
        assertTrue(showsTroubleshooting(await(viewModel.uiState, "the reset offered") { it.canResetSerial }))

        jobs.disconnect()
        assertFalse(showsTroubleshooting(await(viewModel.uiState, "the link gone") { it.link == LinkState.Idle }))

        connector.onConnect = { it.capabilities = """["autostart","files"]""" }
        assertTrue(jobs.connect("car").isSuccess)
        val page = await(viewModel.uiState, "the link again") { it.link is LinkState.Connected }
        assertFalse(page.canResetSerial)
        assertFalse(showsTroubleshooting(page))
    }

    @Test
    fun `a reset from the page ends with the app connected again, and says so in one line`() = test {
        connector.onConnect = { it.capabilities = """["serial-reset"]""" }
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        await(viewModel.uiState, "the reset offered") { it.canResetSerial }

        viewModel.resetSerial()

        val done = await(viewModel.uiState, "the outcome") { it.serialReset is SerialResetUi.Done && it.canResetSerial }
        assertEquals(SerialResetUi.Done("The car ended the Bluetooth link, and the app connected again."), done.serialReset)
        assertEquals(2, connector.agents.size)
    }

    @Test
    fun `each way a reset can go has its own line`() {
        fun line(outcome: SerialResetOutcome) = serialResetText(Result.success(outcome)).text
        assertEquals("The car ended the Bluetooth link, and the app connected again.", line(SerialResetOutcome.Sent(true, true)))
        assertEquals("The car ended the Bluetooth link. The app couldn't connect again by itself.", line(SerialResetOutcome.Sent(true, false)))
        assertEquals(
            "The car didn't end the Bluetooth link in time, so the app ended it and connected again.",
            line(SerialResetOutcome.Sent(false, true)),
        )
        assertEquals(
            "The car didn't end the Bluetooth link in time, so the app ended it. It couldn't connect again by itself.",
            line(SerialResetOutcome.Sent(false, false)),
        )
        assertEquals(
            "No Bluetooth link was up, so the car will end the next one this phone opens within a minute.",
            line(SerialResetOutcome.Armed(60_000)),
        )
        val refused = serialResetText(Result.failure(AgentRefusal("busy", AgentText.serialReset("busy"))))
        assertEquals(SerialResetUi.Done(AgentText.serialReset("busy"), failed = true), refused)
    }
}

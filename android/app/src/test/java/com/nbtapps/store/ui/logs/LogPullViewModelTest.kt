// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.logs

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.FakeLog
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.Refusal
import com.nbtapps.store.data.logs.LogPullState
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.TcpEndpoint
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
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class LogPullViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val connector = FakeConnector()
    private val headUnits = InMemoryHeadUnitRepository()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val jobs by lazy {
        JobRepository(headUnits, connector, identity, MemoryPairingKeys(), PackageStore(folder.newFolder("packages")), {}, scope, shareDir = folder.newFolder("share"))
    }

    private val big = FakeLog("\${data}/log/radio.log", ByteArray(600 * 1024) { ('a' + it % 26).code.toByte() })
    private val small = FakeLog("\${data}/log/radio-1.log", "an older log\n".encodeToByteArray())

    init {
        connector.onConnect = { agent: FakeAgent ->
            agent.capabilities = """["files"]"""
            agent.installed = """[{"id":"com.example.radio","name":"Radio","version":"1.4.0","logs":true}]"""
            agent.logs["com.example.radio"] = listOf(big, small)
            agent.dataChunk = 4000
        }
    }

    @After
    fun tearDown() = scope.cancel()

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144))))
            block()
        }
    }

    private suspend fun connected(): LogPullViewModel {
        assertTrue(jobs.connect("car").isSuccess)
        return LogPullViewModel("car", "com.example.radio", jobs, "0.4.0")
    }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    @Test
    fun `every listed file is chosen to begin with, and the user can take some out`() = test {
        val viewModel = connected()
        val state = await(viewModel.uiState, "the listing") { it.listing is Listing.Ready }
        assertEquals("Radio", state.name)
        assertTrue(state.available)
        assertEquals(setOf(big.path, small.path), state.chosen)
        assertEquals((big.bytes.size + small.bytes.size).toLong(), state.bytes)
        assertTrue(state.canPull)

        viewModel.choose(big.path, false)
        assertEquals(setOf(small.path), viewModel.uiState.value.chosen)
        viewModel.choose(small.path, false)
        assertFalse("nothing to pull", viewModel.uiState.value.canPull)
    }

    @Test
    fun `the tail keeps each file to its end, and the estimate goes by Bluetooth's pace`() = test {
        val viewModel = connected()
        val state = await(viewModel.uiState, "the listing") { it.listing is Listing.Ready }
        // 600 KB and a line at 20 KB/s, rounded up.
        assertEquals(31L, state.estimateSeconds)
        viewModel.setTail(true)
        val tailed = viewModel.uiState.value
        assertEquals(LogPullViewModel.TAIL_BYTES + small.bytes.size, tailed.bytes)
        assertEquals(13L, tailed.estimateSeconds)
        assertEquals("about 13 seconds", duration(13))
        assertEquals("about 3 minutes", duration(121))
    }

    @Test
    fun `a pull started here is handed to the share sheet once, and can be shared again`() = test {
        val viewModel = connected()
        await(viewModel.uiState, "the listing") { it.listing is Listing.Ready }
        viewModel.setTail(true)
        viewModel.start()
        val done = await(viewModel.uiState, "the ZIP to share") { it.shareNow != null }
        val zip = done.shareNow!!
        assertTrue(zip.isFile)
        assertEquals(zip, done.zip)
        assertEquals(2, (done.pull as LogPullState.Done).included)
        val fetch = connector.agent.requestsOf("files.fetch").first()
        assertEquals((big.bytes.size - LogPullViewModel.TAIL_BYTES).toString(), fetch.getValue("offset").jsonPrimitive.content)

        viewModel.shared(zip)
        val after = viewModel.uiState.value
        assertNull("not again by itself", after.shareNow)
        assertEquals("but when asked", zip, after.zip)

        // A screen opened later shows the last pull to share again, and hands nothing on by itself.
        val later = LogPullViewModel("car", "com.example.radio", jobs, "0.4.0")
        val shown = await(later.uiState, "the listing") { it.listing is Listing.Ready }
        assertEquals(zip, shown.zip)
        assertNull(shown.shareNow)
        // Another package's screen shows none of it.
        assertNull(LogPullViewModel("car", "org.nbtapp.agent", jobs, "0.4.0").uiState.value.pull)
    }

    @Test
    fun `a pull while a job runs says to wait for it`() = test {
        connector.onConnect.let { setUp ->
            connector.onConnect = { agent -> setUp(agent); agent.handlers["files.fetch"] = { throw Refusal("busy") } }
        }
        val viewModel = connected()
        await(viewModel.uiState, "the listing") { it.listing is Listing.Ready }
        viewModel.start()
        val failed = await(viewModel.uiState, "the failure") { it.pull is LogPullState.Failed }
        assertEquals("Wait for the install to finish, then try again.", (failed.pull as LogPullState.Failed).reason)
        assertTrue("it can be tried again", failed.canPull)
    }

    @Test
    fun `without a link there is nothing to list or pull`() = test {
        val viewModel = LogPullViewModel("car", "com.example.radio", jobs, "0.4.0")
        val state = await(viewModel.uiState, "the failed listing") { it.listing is Listing.Failed }
        assertFalse(state.available)
        assertFalse(state.canPull)
        assertEquals("Not connected to a head unit", (state.listing as Listing.Failed).reason)
    }
}

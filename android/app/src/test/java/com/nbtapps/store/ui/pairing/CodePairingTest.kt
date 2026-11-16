// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairing

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.InMemoryPrereleaseRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.catalogue.DefaultCatalogueRepository
import com.nbtapps.store.data.catalogue.FileCatalogueSource
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.FakePhones
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.TcpEndpoint
import java.util.concurrent.atomic.AtomicBoolean
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

/** Taking a code from the car's screen and handing it over, as both pairing screens do, against a stand-in agent. */
class CodePairingTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val headUnits = InMemoryHeadUnitRepository()
    private val connector = FakeConnector()
    private val keys = MemoryPairingKeys()
    private val identity = object : PhoneIdentity {
        override suspend fun id() = "5f2c9a1e07b34d6688a0c1d2e3f40516"
        override val name = "Pixel 9"
    }
    private val store by lazy { PackageStore(folder.newFolder("packages")) }
    private val jobs by lazy { JobRepository(headUnits, connector, identity, keys, store, {}, scope, shareDir = folder.newFolder("share")) }
    private val setup by lazy {
        val source = FileCatalogueSource(null, AgentVectors.file("packages/good"))
        AutostartSetup(jobs, DefaultCatalogueRepository(source), store, PackageDownloader(source, store, scope), InMemoryPrereleaseRepository(), scope)
    }
    private val codes by lazy { CodePairing(jobs, setup, scope) }

    private val phones = FakePhones().also { connector.phones = it }

    /** Whether the stand-in car starts the agent with it: an agent with the switch, and the plugin there. */
    private val on = AtomicBoolean(false)

    @After
    fun tearDown() = scope.cancel()

    /** Runs [block] with the link to the car up and waiting for a code, which is [code] on the car's screen. */
    private fun test(code: String = "420042", block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144))))
            phones.code = code
            connector.onConnect = { agent ->
                agent.capabilities = """["autostart"]"""
                agent.handlers["autostart.get"] = { """{"on":${on.get()},"available":true,"starts":0}""" }
                agent.handlers["autostart.set"] = { params ->
                    on.set(params.getValue("on").jsonPrimitive.content == "true")
                    """{"on":${on.get()},"available":true,"starts":0}"""
                }
            }
            assertTrue(jobs.connect("car").isFailure)
            assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)
            block()
        }
    }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    @Test
    fun `the code is its digits as typed, and Pair waits until it is whole`() = test {
        codes.setCode("42 00 4")
        assertEquals(CodeEntry(code = "42004"), codes.state.value)
        assertFalse(codes.state.value.complete)
        codes.pair("car", autostart = false)
        assertFalse("nothing is sent before the code is whole", codes.state.value.working)

        codes.setCode("420-0428")
        assertEquals("420042", codes.state.value.code)
        assertTrue(codes.state.value.complete)
    }

    @Test
    fun `a right code pairs the phone, and with the box cleared no set-up is begun`() = test {
        codes.setCode("420042")
        codes.pair("car", autostart = false)

        val done = await(codes.state, "the answer") { !it.working }
        assertEquals(CodeEntry(), done)
        assertEquals("car", (jobs.connection.value as ConnectionState.Connected).headUnitId)
        assertNull(setup.state.value)
    }

    @Test
    fun `with the box ticked and offered the set-up begins once the phone is paired`() = test {
        codes.setCode("420042")
        codes.pair("car", autostart = true)

        await(codes.state, "the answer") { !it.working }
        assertEquals("car", await(setup.state, "the set-up") { it != null }?.headUnitId)
        assertTrue(await(setup.state, "the set-up to end") { it?.ended == true }!!.step.toString(), on.get())
    }

    @Test
    fun `a wrong code is cleared and says why, and the link goes on waiting for the next`() = test {
        codes.setCode("111111")
        codes.pair("car", autostart = true)

        val refused = await(codes.state, "the refusal") { !it.working && it.error != null }
        assertEquals(CodeEntry(error = AgentText.pairing("rejected")), refused)
        assertTrue(jobs.connection.value is ConnectionState.NeedsPairing)
        assertNull("no set-up for a phone that is not paired", setup.state.value)

        // Typing again takes the error away.
        codes.setCode("4")
        assertEquals(CodeEntry(code = "4"), codes.state.value)
    }
}

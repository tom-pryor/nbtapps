// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.settings.SettingsChanges
import com.nbtapps.store.data.settings.SettingsException
import com.nbtapps.store.data.settings.SettingsFailure
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.SettingFault
import com.nbtapps.store.model.SettingState
import com.nbtapps.store.model.SettingValue
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.withTestTimeout
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.job
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** A package's settings read and saved through [JobRepository], over the link it holds, against a stand-in agent. */
class AgentSettingsTest {

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
        JobRepository(headUnits, connector, identity, MemoryPairingKeys(), PackageStore(folder.newFolder("packages")), {}, scope)
    }

    private val dial = FakeSettings(
        "org.nbtapp.dial",
        listOf(
            """{"key":"CHIME","label":"Chime","type":"bool","default":true,"applies":"now","advanced":false}""",
            """{"key":"DIAL_SPEED","label":"Dial speed","type":"int","min":1,"max":50,"default":10,"applies":"app-start","advanced":false}""",
            """{"key":"GREETING","label":"Greeting","type":"text","maxLength":40,"characters":"any","applies":"reboot","advanced":false}""",
            """{"key":"LAMP_HOST","label":"Lamp address","type":"text","maxLength":64,"characters":"safe","unset":"No lamp","applies":"now","advanced":true}""",
            """{"key":"FACE","label":"Dial face","type":"select","values":[{"value":"round","label":"Round"},{"value":"hex","label":"Hexagon"}],""" +
                """"default":"round","applies":"hmi-restart","advanced":false}""",
        ),
    )

    init {
        connector.onConnect = { agent ->
            agent.capabilities = """["files","settings"]"""
            agent.installed = """[{"id":"org.nbtapp.dial","name":"Dial","version":"1.0.0","settings":true}]"""
            agent.settings = dial
        }
    }

    /** Ends the links too, which a cancel does not: their reader threads would outlive the test otherwise. */
    @After
    fun tearDown() = runBlocking {
        scope.cancel()
        connector.agents.forEach { it.drop() }
        withTimeout(5_000) { scope.coroutineContext.job.join() }
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTestTimeout(60_000) {
            headUnits.add(HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144))))
            block()
        }
    }

    private fun failure(result: Result<*>): SettingsException = result.exceptionOrNull() as? SettingsException
        ?: throw AssertionError("not a failure of settings: $result")

    @Test
    fun `every page is read, and the packages list says which have settings`() = test {
        dial.values["DIAL_SPEED"] = "25"
        dial.byHand["FACE"] = "oval"
        dial.pageSize = 2
        assertTrue(jobs.connect("car").isSuccess)
        assertTrue((jobs.connection.value as ConnectionState.Connected).settings)
        assertTrue((jobs.connection.value as ConnectionState.Connected).installed.single().settings)

        val entries = jobs.loadSettings("car", "org.nbtapp.dial").getOrThrow()
        assertEquals(listOf("CHIME", "DIAL_SPEED", "GREETING", "LAMP_HOST", "FACE"), entries.map { it.option.key })
        assertEquals(listOf(null, "2", "4"), connector.agent.requestsOf("settings.get").map { it["from"]?.jsonPrimitive?.content })
        assertEquals(SettingValue.Number(25), entries[1].value)
        assertEquals(SettingState.BY_HAND, entries[4].state)
        assertEquals("oval", entries[4].raw)
        assertEquals(SettingState.UNSET, entries[0].state)
    }

    @Test
    fun `changes that fit one frame are saved with one request`() = test {
        assertTrue(jobs.connect("car").isSuccess)
        val changes = SettingsChanges(mapOf("CHIME" to SettingValue.Bool(false), "GREETING" to SettingValue.Text("Hi é")), listOf("FACE"))
        assertTrue(jobs.saveSettings("car", "org.nbtapp.dial", changes).isSuccess)
        val sent = dial.sets.single()
        assertEquals("""{"CHIME":false,"GREETING":"Hi é"}""", sent["set"].toString())
        assertEquals("""["FACE"]""", sent["reset"].toString())
        assertEquals(SettingValue.Bool(false), jobs.loadSettings("car", "org.nbtapp.dial").getOrThrow().first().value)
    }

    @Test
    fun `changes too large for one frame go in parts, and a part that fails says what was saved before it`() = test {
        val many = FakeSettings("org.nbtapp.many", (1..30).map { """{"key":"TEXT_$it","label":"Text $it","type":"text","maxLength":255,"characters":"any"}""" })
        connector.onConnect = { agent ->
            agent.capabilities = """["settings"]"""
            agent.maxFrame = 4096
            agent.settings = many
        }
        assertTrue(jobs.connect("car").isSuccess)
        val changes = SettingsChanges((1..30).associate { "TEXT_$it" to SettingValue.Text("t".repeat(250)) })

        assertTrue(jobs.saveSettings("car", "org.nbtapp.many", changes).isSuccess)
        assertTrue("${many.sets.size} requests", many.sets.size in 2..4)
        assertEquals(changes.set.keys.toList(), many.sets.flatMap { (it["set"] as JsonObject).keys })
        assertEquals(30, many.values.size)

        // Again, with the second part refused: the first part's options are saved, and said to be.
        many.sets.clear()
        many.values.clear()
        many.refuseSet = "busy"
        many.setsBeforeRefusal = 1
        val failed = failure(jobs.saveSettings("car", "org.nbtapp.many", changes))
        assertEquals(SettingsFailure.Busy, failed.failure)
        val first = (many.sets.first()["set"] as JsonObject).keys
        assertEquals(first, failed.saved)
        assertEquals(first, many.values.keys)
        assertEquals("nothing after the part that failed", 2, many.sets.size)
    }

    @Test
    fun `a refusal says which keys and why, and nothing of that request is saved`() = test {
        dial.faults["DIAL_SPEED"] = "out-of-range"
        assertTrue(jobs.connect("car").isSuccess)
        val changes = SettingsChanges(mapOf("CHIME" to SettingValue.Bool(false), "DIAL_SPEED" to SettingValue.Number(51), "NOPE" to SettingValue.Number(1)))
        val failed = failure(jobs.saveSettings("car", "org.nbtapp.dial", changes))
        assertEquals(
            SettingsFailure.Invalid(listOf(SettingFault("DIAL_SPEED", "out-of-range"), SettingFault("NOPE", "unknown-key"))),
            failed.failure,
        )
        assertTrue(failed.saved.isEmpty())
        assertTrue(dial.values.isEmpty())
        assertEquals("The head unit refused 2 changes", failed.message)
    }

    @Test
    fun `the settings of a head unit are neither read nor saved over a link to another`() = test {
        headUnits.add(HeadUnit("van", "Test van", listOf(TcpEndpoint("192.168.0.2", 7144))))
        assertTrue(jobs.connect("car").isSuccess)
        assertEquals(SettingsFailure.NotConnected, failure(jobs.loadSettings("van", "org.nbtapp.dial")).failure)
        val changes = SettingsChanges(mapOf("CHIME" to SettingValue.Bool(false)))
        assertEquals(SettingsFailure.NotConnected, failure(jobs.saveSettings("van", "org.nbtapp.dial", changes)).failure)
        assertTrue("nothing was sent", connector.agent.requestsOf("settings.get").isEmpty() && dial.sets.isEmpty())
        // The head unit connected is still served.
        assertTrue(jobs.saveSettings("car", "org.nbtapp.dial", changes).isSuccess)
    }

    @Test
    fun `each other failure is one the screen knows`() = test {
        assertEquals(SettingsFailure.NotConnected, failure(jobs.loadSettings("car", "org.nbtapp.dial")).failure)
        assertTrue(jobs.connect("car").isSuccess)
        assertEquals(SettingsFailure.NotFound, failure(jobs.loadSettings("car", "org.nbtapp.other")).failure)
        dial.refuseGet = "busy"
        assertEquals(SettingsFailure.Busy, failure(jobs.loadSettings("car", "org.nbtapp.dial")).failure)
        dial.refuseGet = "io-error"
        assertEquals(
            SettingsFailure.Failed("The head unit couldn't read or write the package's settings file"),
            failure(jobs.loadSettings("car", "org.nbtapp.dial")).failure,
        )
        dial.refuseGet = "unsupported"
        assertEquals(SettingsFailure.TooOld, failure(jobs.loadSettings("car", "org.nbtapp.dial")).failure)
        dial.refuseGet = null

        // A job that runs on the head unit: nothing is sent.
        connector.agent.event("job.progress", 1, """{"planId":"0102030405060708","state":"installing","step":1,"steps":1,"id":"org.nbtapp.dial"}""")
        jobs.job.first { it != null }
        val busy = failure(jobs.saveSettings("car", "org.nbtapp.dial", SettingsChanges(mapOf("CHIME" to SettingValue.Bool(false)))))
        assertEquals(SettingsFailure.Busy, busy.failure)
        assertTrue(dial.sets.isEmpty())

        // An agent from before settings.
        jobs.disconnect()
        connector.onConnect = { agent -> agent.settings = dial }
        assertTrue(jobs.connect("car").isSuccess)
        assertEquals(SettingsFailure.TooOld, failure(jobs.loadSettings("car", "org.nbtapp.dial")).failure)

        // The link ends while the answer is awaited.
        jobs.disconnect()
        connector.onConnect = { agent ->
            agent.capabilities = """["settings"]"""
            agent.handlers["settings.get"] = {
                agent.drop()
                throw Refusal("busy")
            }
        }
        assertTrue(jobs.connect("car").isSuccess)
        assertTrue(failure(jobs.loadSettings("car", "org.nbtapp.dial")).failure is SettingsFailure.LinkLost)
    }
}

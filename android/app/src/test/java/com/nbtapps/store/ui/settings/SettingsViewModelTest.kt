// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.settings

import androidx.lifecycle.viewModelScope
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.PhoneIdentity
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.FakeSettings
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.settings.SettingRules
import com.nbtapps.store.data.settings.SettingsFailure
import com.nbtapps.store.model.FreeSpace
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.SettingApplies
import com.nbtapps.store.model.TcpEndpoint
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.job
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The settings screen's states, against a stand-in agent behind the app's own repository. */
class SettingsViewModelTest {

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
        JobRepository(headUnits, connector, identity, MemoryPairingKeys(), PackageStore(folder.newFolder("packages")), {}, scope)
    }
    private val viewModels = CopyOnWriteArrayList<SettingsViewModel>()

    private val dial = FakeSettings(
        "org.nbtapp.dial",
        listOf(
            """{"key":"CHIME","label":"Chime","description":"Play a chime when the dial turns.","type":"bool","default":true,"applies":"now","advanced":false}""",
            """{"key":"DIAL_SPEED","label":"Dial speed","type":"int","min":1,"max":50,"unit":"°/s","default":10,"applies":"app-start","advanced":false}""",
            """{"key":"GREETING","label":"Greeting","type":"text","maxLength":40,"characters":"any","default":"Hello","applies":"app-start","advanced":false}""",
            """{"key":"LAMP_HOST","label":"Lamp address","type":"text","maxLength":64,"characters":"safe","unset":"No lamp","applies":"now","advanced":true}""",
            """{"key":"FACE","label":"Dial face","type":"select","values":[{"value":"round","label":"Round"},{"value":"hex","label":"Hexagon"}],""" +
                """"default":"round","applies":"hmi-restart","advanced":true}""",
        ),
    )

    init {
        connector.onConnect = { agent ->
            agent.capabilities = """["settings"]"""
            agent.installed = """[{"id":"org.nbtapp.dial","name":"Dial","version":"1.0.0","settings":true}]"""
            agent.settings = dial
        }
    }

    /** The latches the tests hold the stand-in agents' answers on: each is let go at the end, should a test fail first. */
    private val held = CopyOnWriteArrayList<CountDownLatch>()

    private fun latch() = CountDownLatch(1).also { held += it }

    /**
     * Waits for the view models and the links to end, not only cancels them. An answer that comes as a test ends is
     * taken on the link's reader thread, and the screen's coroutines that it wakes run on there: one that is cancelled
     * meanwhile still reaches for the main dispatcher as it ends, which the rule unsets once this returns, and the next
     * test that uses `runTest` then fails with it.
     */
    @After
    fun tearDown() = runBlocking {
        held.forEach { it.countDown() }
        viewModels.forEach { it.viewModelScope.cancel() }
        scope.cancel()
        // A cancel does not end a read that waits on a link: the stand-in agents end their side of each.
        connector.agents.forEach { it.drop() }
        withTimeout(5_000) {
            viewModels.forEach { it.viewModelScope.coroutineContext.job.join() }
            scope.coroutineContext.job.join()
        }
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTimeout(60_000) {
            headUnits.add(HeadUnit("car", "Test car", listOf(TcpEndpoint("192.168.0.1", 7144))))
            block()
        }
    }

    private fun screen() = SettingsViewModel("car", "org.nbtapp.dial", jobs, headUnits).also { viewModels += it }

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private suspend fun loaded(): SettingsViewModel {
        assertTrue(jobs.connect("car").isSuccess)
        val viewModel = screen()
        await(viewModel.uiState, "the settings") { it.edit != null }
        return viewModel
    }

    private fun SettingsUiState.row(key: String): OptionRow = rows.first { it.key == key }

    @Test
    fun `the settings are read when the screen opens, and each row says how its option stands`() = test {
        dial.values["DIAL_SPEED"] = "25"
        dial.byHand["FACE"] = "oval"
        val reading = latch()
        connector.onConnect.let { setUp ->
            connector.onConnect = { agent ->
                setUp(agent)
                agent.handlers["settings.get"] = { params ->
                    reading.await(10, TimeUnit.SECONDS)
                    dial.get(params)
                }
            }
        }
        assertTrue(jobs.connect("car").isSuccess)
        val viewModel = screen()
        val loading = await(viewModel.uiState, "the reading") { it.connected }
        assertEquals(SettingsLoad.Loading, loading.load)
        assertNull(loading.edit)
        reading.countDown()

        val state = await(viewModel.uiState, "the settings") { it.edit != null }
        assertEquals("Dial", state.name)
        assertEquals(listOf("CHIME", "DIAL_SPEED", "GREETING", "LAMP_HOST", "FACE"), state.rows.map { it.key })
        assertNull("they take effect at different times", state.takesEffect)

        val chime = state.row("CHIME")
        assertTrue(chime.on && chime.muted && !chime.canReset)
        assertEquals("Not set · uses on", chime.meta)
        assertEquals("Takes effect at once", chime.takesEffect)
        val speed = state.row("DIAL_SPEED")
        assertEquals("25", speed.input)
        assertEquals("Default: 10 °/s · 1 to 50", speed.meta)
        assertTrue(speed.canReset && !speed.muted)
        val greeting = state.row("GREETING")
        assertEquals("", greeting.input)
        assertEquals("Hello", greeting.placeholder)
        assertEquals("Not set · uses Hello · up to 40 bytes", greeting.meta)
        assertEquals("Not set · No lamp · up to 64 characters", state.row("LAMP_HOST").meta)
        val face = state.row("FACE")
        assertTrue(face.byHand && face.canReset && face.advanced)
        assertEquals("oval", face.input)
        assertEquals("Default: Round", face.meta)
        assertFalse(state.advancedOpen)
        assertEquals(0, state.advancedChanged)
    }

    @Test
    fun `changes wait in the bar, the advanced header counts its own, and Save sends them and says when they take effect`() = test {
        val viewModel = loaded()
        viewModel.set("CHIME", SettingRules.OFF)
        viewModel.set("DIAL_SPEED", "30")
        viewModel.set("FACE", "hex")
        var state = await(viewModel.uiState, "three changes") { it.count == 3 }
        assertTrue(state.canSave)
        assertEquals("Not saved yet", saveBarLine(state))
        assertEquals(1, state.advancedChanged)
        assertEquals("2 options · 1 changed", advancedLine(2, state.advancedChanged))
        val speed = state.row("DIAL_SPEED")
        assertTrue(speed.changed)
        assertEquals("Changed · was not set · 1 to 50", speed.meta)

        // A change taken back, and Discard, leave nothing to save.
        viewModel.set("DIAL_SPEED", "")
        assertEquals(2, await(viewModel.uiState, "the speed as it was") { it.count == 2 }.count)
        viewModel.discard()
        assertFalse(await(viewModel.uiState, "nothing changed") { it.count == 0 }.canSave)

        viewModel.set("CHIME", SettingRules.OFF)
        viewModel.set("DIAL_SPEED", "30")
        viewModel.set("FACE", "hex")
        await(viewModel.uiState, "three changes again") { it.count == 3 }
        viewModel.save()
        state = await(viewModel.uiState, "the saved card") { it.saved != null && !it.saving }
        assertEquals(0, state.count)
        assertEquals(
            listOf(SavedGroup(SettingApplies.NOW, 1), SavedGroup(SettingApplies.APP_START, 1), SavedGroup(SettingApplies.HMI_RESTART, 1)),
            state.saved,
        )
        assertEquals(
            listOf("1 change is in effect now.", "1 change takes effect when Dial is next started.", "1 change takes effect when iDrive next restarts."),
            state.saved!!.map { savedLine(it, "Dial") },
        )
        assertEquals(mapOf("CHIME" to "false", "DIAL_SPEED" to "30", "FACE" to "\"hex\""), dial.values.toMap())
        // Read again: they are set now.
        state = await(viewModel.uiState, "the settings read again") { it.edit?.entries?.first()?.value != null }
        assertEquals("Default: on", state.row("CHIME").meta)
        assertTrue(state.row("CHIME").canReset)
    }

    @Test
    fun `a field keeps what is typed into it, where it passes through the default too`() = test {
        val viewModel = loaded()
        viewModel.set("DIAL_SPEED", "1")
        viewModel.set("DIAL_SPEED", "10")
        var state = await(viewModel.uiState, "the default typed") { it.row("DIAL_SPEED").input == "10" }
        var speed = state.row("DIAL_SPEED")
        assertEquals(0, state.count)
        assertFalse(speed.changed || speed.muted || speed.canReset)
        assertEquals("Not set · uses 10 °/s · 1 to 50", speed.meta)
        viewModel.set("DIAL_SPEED", "100")
        state = await(viewModel.uiState, "a third digit") { it.row("DIAL_SPEED").input == "100" }
        speed = state.row("DIAL_SPEED")
        assertEquals(1, state.count)
        assertTrue(speed.changed)
        assertEquals("Use a number from 1 to 50", speed.error)

        viewModel.set("GREETING", "Hello")
        state = await(viewModel.uiState, "the default text") { it.row("GREETING").input == "Hello" }
        assertFalse(state.row("GREETING").changed)
        viewModel.set("GREETING", "Hello!")
        state = await(viewModel.uiState, "more text") { it.row("GREETING").input == "Hello!" }
        assertEquals("Changed · was not set · up to 40 bytes", state.row("GREETING").meta)
        assertEquals(2, state.count)
    }

    @Test
    fun `a value the phone's check refuses is shown under its option, and nothing can be saved meanwhile`() = test {
        val viewModel = loaded()
        viewModel.set("DIAL_SPEED", "51")
        viewModel.set("LAMP_HOST", "a b")
        val state = await(viewModel.uiState, "the problems") { it.count == 2 }
        assertFalse(state.canSave)
        assertEquals("Correct the values in red to save", saveBarLine(state))
        assertEquals("Use a number from 1 to 50", state.row("DIAL_SPEED").error)
        assertEquals("Only letters A to Z, digits and . _ : / @ + , - are allowed", state.row("LAMP_HOST").error)
        viewModel.save()
        assertTrue(dial.sets.isEmpty())
    }

    @Test
    fun `a refusal from the head unit is shown under the option at fault, and every change is kept`() = test {
        dial.faults["FACE"] = "not-a-choice"
        val viewModel = loaded()
        viewModel.set("CHIME", SettingRules.OFF)
        viewModel.set("FACE", "hex")
        await(viewModel.uiState, "two changes") { it.count == 2 }
        viewModel.save()
        val state = await(viewModel.uiState, "the refusal") { it.refused.isNotEmpty() && !it.saving }
        assertEquals(2, state.count)
        assertNull(state.saved)
        assertEquals("Nothing was saved: the head unit refused one change. Correct it and save again; your other changes are kept.", state.error)
        assertEquals("The head unit refused this: not one of the choices.", state.row("FACE").error)
        assertNull(state.row("CHIME").error)
        assertTrue("the advanced option at fault is shown", state.advancedOpen)
        assertTrue(dial.values.isEmpty())
        // Changing the option at fault takes its refusal away.
        viewModel.set("FACE", "round")
        assertNull(await(viewModel.uiState, "the refusal gone") { it.refused.isEmpty() }.row("FACE").error)
    }

    @Test
    fun `while an install runs the changes are kept and cannot be saved`() = test {
        val viewModel = loaded()
        viewModel.set("CHIME", SettingRules.OFF)
        await(viewModel.uiState, "a change") { it.count == 1 }
        val agent = connector.agent
        agent.event("job.progress", 1, """{"planId":"0102030405060708","state":"installing","step":1,"steps":1,"id":"org.nbtapp.other"}""")
        var state = await(viewModel.uiState, "the install") { it.jobRunning }
        assertFalse(state.canSave)
        assertEquals("Save when the install has finished", saveBarLine(state))
        assertEquals(1, state.count)
        agent.event("job.done", 2, """{"planId":"0102030405060708","state":"succeeded","steps":[],"restart":"none"}""")
        state = await(viewModel.uiState, "the install's end") { !it.jobRunning }
        assertTrue(state.canSave)
    }

    @Test
    fun `settings that could not be read during an install are read once it has finished`() = test {
        dial.refuseGet = "busy"
        assertTrue(jobs.connect("car").isSuccess)
        val viewModel = screen()
        val failed = await(viewModel.uiState, "the refusal") { it.load is SettingsLoad.Failed }
        assertEquals(SettingsLoad.Failed(SettingsFailure.Busy), failed.load)
        dial.refuseGet = null
        connector.agent.event("job.progress", 1, """{"planId":"0102030405060708","state":"installing","step":1,"steps":1,"id":"org.nbtapp.other"}""")
        await(viewModel.uiState, "the install") { it.jobRunning }
        connector.agent.event("job.done", 2, """{"planId":"0102030405060708","state":"succeeded","steps":[],"restart":"none"}""")
        await(viewModel.uiState, "the settings") { it.edit != null }
    }

    @Test
    fun `a link lost while editing keeps the changes, and they can be saved once it is back`() = test {
        val viewModel = loaded()
        viewModel.set("DIAL_SPEED", "30")
        await(viewModel.uiState, "a change") { it.count == 1 }
        connector.agent.drop()
        var state = await(viewModel.uiState, "the link gone") { it.linkLost }
        assertFalse(state.canSave)
        assertEquals(1, state.count)
        assertEquals("Not connected", saveBarLine(state))

        // Meanwhile the speed was set on the head unit to what the change gives it, and the face set.
        dial.values["FACE"] = "\"hex\""
        assertTrue(jobs.connect("car").isSuccess)
        state = await(viewModel.uiState, "the settings read again") { it.connected && it.edit?.entries?.last()?.value != null }
        assertEquals(1, state.count)
        assertTrue(state.canSave)
        viewModel.save()
        await(viewModel.uiState, "the saved card") { it.saved != null }
        assertEquals("30", dial.values["DIAL_SPEED"])
    }

    @Test
    fun `without a link there is nothing to read, and an agent without settings asks for an update`() = test {
        headUnits.update("car") {
            it.copy(profile = HeadUnitProfile("1.4.1", 1, FreeSpace(), "none", listOf(InstalledPackage("org.nbtapp.dial", "Dial", "1.0.0", settings = true)), 1))
        }
        val offline = screen()
        val state = await(offline.uiState, "the name from the saved profile") { it.name == "Dial" }
        assertFalse(state.connected)
        assertNull(state.edit)
        assertFalse(state.canSave)

        connector.onConnect = { agent -> agent.settings = dial }
        assertTrue(jobs.connect("car").isSuccess)
        val old = await(offline.uiState, "the agent") { it.connected }
        assertTrue(old.tooOld)
        assertNull(old.edit)
        assertTrue(connector.agent.requestsOf("settings.get").isEmpty())
    }

    /** Has every agent from now on answer [method] with [handler] instead of the fake's own. */
    private fun answer(method: String, handler: (JsonObject) -> String) {
        connector.onConnect.let { setUp ->
            connector.onConnect = { agent ->
                setUp(agent)
                agent.handlers[method] = handler
            }
        }
    }

    /**
     * Has the [held]th `settings.get` answer with the settings as they are when it comes, but only once [release] is
     * counted down; [answered] is counted down as that answer goes. The others answer at once.
     */
    private fun holdRead(held: Int, release: CountDownLatch, answered: CountDownLatch): AtomicInteger {
        val reads = AtomicInteger()
        answer("settings.get") { params ->
            val text = dial.get(params)
            if (reads.incrementAndGet() == held) {
                release.await(10, TimeUnit.SECONDS)
                answered.countDown()
            }
            text
        }
        return reads
    }

    private suspend fun until(what: String, condition: () -> Boolean) {
        withTimeoutOrNull(10_000) { while (!condition()) delay(10) } ?: throw AssertionError("never came: $what")
    }

    @Test
    fun `a change made while a save goes on is kept, and the one saved leaves the bar`() = test {
        val release = latch()
        answer("settings.set") { params ->
            release.await(10, TimeUnit.SECONDS)
            dial.set(params)
        }
        val viewModel = loaded()
        viewModel.set("CHIME", SettingRules.OFF)
        viewModel.set("DIAL_SPEED", "30")
        await(viewModel.uiState, "two changes") { it.count == 2 }
        viewModel.save()
        await(viewModel.uiState, "the save under way") { it.saving }
        viewModel.set("DIAL_SPEED", "35")
        await(viewModel.uiState, "the speed changed again") { it.row("DIAL_SPEED").input == "35" }
        release.countDown()

        val state = await(viewModel.uiState, "the saved card") { it.saved != null && !it.saving }
        assertEquals("30", dial.values["DIAL_SPEED"])
        assertEquals(1, state.count)
        val speed = state.row("DIAL_SPEED")
        assertEquals("35", speed.input)
        assertTrue(speed.changed)
        assertEquals("Changed · was 30 °/s · 1 to 50", speed.meta)
        assertFalse(state.row("CHIME").changed)
        // And once the settings are read again.
        until("the read after the save") { connector.agent.requestsOf("settings.get").size == 2 }
        val lost = withTimeoutOrNull(1_000) { viewModel.uiState.first { it.count != 1 || it.row("DIAL_SPEED").input != "35" } }
        assertNull("the change is still there", lost)
    }

    @Test
    fun `what was saved is shown even where the settings cannot be read again`() = test {
        dial.values["FACE"] = "\"hex\""
        val viewModel = loaded()
        dial.refuseGet = "io-error"
        viewModel.set("DIAL_SPEED", "30")
        viewModel.reset("FACE")
        await(viewModel.uiState, "two changes") { it.count == 2 }
        viewModel.save()
        await(viewModel.uiState, "the saved card") { it.saved != null && !it.saving }
        until("the read after the save") { connector.agent.requestsOf("settings.get").size == 2 }

        val state = viewModel.uiState.value
        assertEquals(0, state.count)
        val speed = state.row("DIAL_SPEED")
        assertEquals("30", speed.input)
        assertEquals("Default: 10 °/s · 1 to 50", speed.meta)
        assertTrue(speed.canReset && !speed.changed)
        val face = state.row("FACE")
        assertEquals("", face.input)
        assertTrue(face.muted && !face.canReset)
        assertEquals("Not set · uses Round", face.meta)
    }

    @Test
    fun `of reads that overlap, only the last one begun is taken`() = test {
        val release = latch()
        val answered = CountDownLatch(1)
        val reads = holdRead(2, release, answered)
        val viewModel = loaded()
        dial.values["DIAL_SPEED"] = "25"
        viewModel.load()
        until("the second read") { reads.get() == 2 }
        dial.values["DIAL_SPEED"] = "40"
        viewModel.load()
        await(viewModel.uiState, "the last read") { it.row("DIAL_SPEED").input == "40" }

        release.countDown()
        assertTrue(answered.await(10, TimeUnit.SECONDS))
        val stale = withTimeoutOrNull(1_000) { viewModel.uiState.first { it.row("DIAL_SPEED").input != "40" } }
        assertNull("the earlier read's answer is not taken", stale)
    }

    @Test
    fun `a read begun before a save does not put back what was there before it`() = test {
        val release = latch()
        val answered = CountDownLatch(1)
        val reads = holdRead(2, release, answered)
        val viewModel = loaded()
        viewModel.load()
        until("the second read") { reads.get() == 2 }
        viewModel.set("DIAL_SPEED", "30")
        await(viewModel.uiState, "a change") { it.count == 1 }
        viewModel.save()
        await(viewModel.uiState, "the saved card") { it.saved != null && !it.saving }
        until("the read after the save") { reads.get() == 3 }

        release.countDown()
        assertTrue(answered.await(10, TimeUnit.SECONDS))
        val stale = withTimeoutOrNull(1_000) { viewModel.uiState.first { it.row("DIAL_SPEED").input != "30" } }
        assertNull("the read from before the save is not taken", stale)
        assertEquals("Default: 10 °/s · 1 to 50", viewModel.uiState.value.row("DIAL_SPEED").meta)
    }

    @Test
    fun `a reset takes a set option back to not set`() = test {
        dial.values["DIAL_SPEED"] = "25"
        val viewModel = loaded()
        viewModel.reset("DIAL_SPEED")
        val state = await(viewModel.uiState, "the reset") { it.count == 1 }
        val speed = state.row("DIAL_SPEED")
        assertTrue(speed.muted && speed.changed && !speed.canReset)
        assertEquals("", speed.input)
        assertEquals("10", speed.placeholder)
        assertEquals("Changed · back to not set · uses 10 °/s · 1 to 50", speed.meta)
        viewModel.save()
        await(viewModel.uiState, "the saved card") { it.saved != null }
        assertNull(dial.values["DIAL_SPEED"])
    }
}

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
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.catalogue.DefaultCatalogueRepository
import com.nbtapps.store.data.catalogue.FileCatalogueSource
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.FakeChannel
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.Refusal
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CataloguePackage
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.FreeSpace
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.PackageRequirements
import com.nbtapps.store.model.PackageVersion
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.withTestTimeout
import java.io.IOException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * The Debug card on the head unit's page: when it is there, its seven states, the switch and its question, each
 * refusal, and the terminal's row. Over the fake agent, whose SSH package's channel stands in for the real one.
 */
class HeadUnitDebugCardTest {

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
    private val terminal = MutableStateFlow<TerminalState>(TerminalState.Idle)
    private val catalogue = MutableStateFlow(CatalogueState())

    private val car = HeadUnit("car", "Test car", listOf(SerialEndpoint("00:11:22:33:44:55", "Test car")))

    /** The SSH package's channel on the fake agent: switched, off until switched on. */
    private val channel = FakeChannel(SshTunnel.PACKAGE, SshTunnel.CHANNEL, switched = true)

    private val sshInstalled = """[{"id":"${SshTunnel.PACKAGE}","name":"SSH access","version":"1.0.0"}]"""

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) {
        withTestTimeout(60_000) {
            headUnits.add(car)
            block()
        }
    }

    /** Every agent from here on has channels, with [installed] and the SSH channel. */
    private fun agents(installed: String = sshInstalled, channels: Boolean = true) {
        connector.onConnect = { agent ->
            agent.capabilities = if (channels) """["channels"]""" else "[]"
            agent.installed = installed
            agent.addChannel(channel)
        }
    }

    private fun viewModel() = mainDispatcherRule.track(
        HeadUnitDetailViewModel(
            "car", headUnits, jobs, connector, FakePhoneBluetooth(), setup, scope,
            terminal = terminal,
            catalogue = catalogue,
        ),
    )

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private suspend fun HeadUnitDetailViewModel.card(what: String, predicate: (DebugCard) -> Boolean): DebugCard =
        await(uiState, what) { it.debug?.let(predicate) == true }.debug!!

    /** The requests of [method] the agent of the link has had. */
    private fun requests(method: String) = connector.agent.requests.count { it.first == method }

    private fun sshVersion(requires: String = ">=1.7.0-beta.1") = PackageVersion(
        version = "1.0.0",
        url = "https://example.invalid/ssh-1.0.0.nbtpkg",
        size = 1,
        sha256 = "00",
        manifestSha256 = "11",
        requires = PackageRequirements(agent = requires, app = ">=0.9.0"),
    )

    private fun offering(vararg versions: PackageVersion) = CatalogueState(
        CatalogueIndex(
            schema = 1,
            source = CatalogueSourceInfo("test", "Test"),
            serial = 1,
            packages = listOf(CataloguePackage(id = SshTunnel.PACKAGE, name = "SSH access", versions = versions.toList())),
        ),
    )

    // ---- When the card is there ----

    @Test
    fun `the card is there with a link to an agent that has channels and the package, or the catalogue's offer of it`() {
        val connected = HeadUnitDetailUiState(link = LinkState.Connected("TCP"), channels = true)
        val withSsh = listOf(InstalledPackage(SshTunnel.PACKAGE, "SSH access", "1.0.0"))
        val none = SshSwitchUi()

        assertNull(debugCard(connected, offered = false, none, TerminalState.Idle, "car"))
        assertEquals(DebugCard(installed = false), debugCard(connected, offered = true, none, TerminalState.Idle, "car"))
        assertEquals(DebugCard(installed = true), debugCard(connected.copy(installed = withSsh), offered = false, none, TerminalState.Idle, "car"))
        // An agent without channels has no card, whatever is installed or offered.
        assertNull(debugCard(connected.copy(channels = false, installed = withSsh), offered = true, none, TerminalState.Idle, "car"))
        // With no link: only where the package was there when the agent last said, and the switch is not known then.
        val apart = HeadUnitDetailUiState(link = LinkState.Idle, installed = withSsh)
        assertEquals(DebugCard(installed = true), debugCard(apart, offered = false, SshSwitchUi(on = true, connections = 1), TerminalState.Idle, "car"))
        assertNull(debugCard(apart.copy(installed = emptyList()), offered = true, none, TerminalState.Idle, "car"))
    }

    @Test
    fun `the catalogue offers the package where a version of it runs on the agent`() {
        assertFalse(sshOffered(CatalogueState(), emptyList(), "1.7.0-beta.1", emptySet()))
        assertTrue(sshOffered(offering(sshVersion()), emptyList(), "1.7.0-beta.1", emptySet()))
        assertFalse(sshOffered(offering(sshVersion(">=2.0.0")), emptyList(), "1.7.0-beta.1", emptySet()))
    }

    @Test
    fun `an agent with channels and no package shows the card only where the catalogue offers the package`() = test {
        agents(installed = "[]")
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        assertNull(await(viewModel.uiState, "the link") { it.link is LinkState.Connected && it.channels }.debug)

        // A version that the agent, 1.0.0 here, cannot run is no offer.
        catalogue.value = offering(sshVersion(">=1.7.0-beta.1"))
        delay(200)
        assertNull(viewModel.uiState.value.debug)

        catalogue.value = offering(sshVersion(">=1.0.0"))
        // State 1: one row, which offers the install.
        val card = viewModel.card("the offer") { true }
        assertEquals(DebugCard(installed = false), card)
        assertEquals(0, requests("channel.get"))
    }

    @Test
    fun `an agent without channels has no card`() = test {
        agents(channels = false)
        catalogue.value = offering(sshVersion())
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        val page = await(viewModel.uiState, "the link") { it.link is LinkState.Connected && it.installed.isNotEmpty() }
        assertNull(page.debug)
    }

    // ---- The states ----

    @Test
    fun `installed and off, then on, then a shell open, with the switch read when the link comes up and the terminal changes`() = test {
        agents()
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)

        // State 2: off; Open is greyed until it is on.
        val off = viewModel.card("off") { it.on == false }
        assertEquals(SSH_OFF_AFTER_RESTART, off.switchLine)
        assertTrue(off.canSwitch)
        assertFalse(off.canOpenTerminal)

        // State 4: on, read again once the head unit has it on and the terminal changes.
        channel.on = true
        terminal.value = TerminalState.Connecting("car")
        val on = viewModel.card("on") { it.on == true }
        assertTrue(on.canOpenTerminal)
        assertTrue(on.terminalOpen)

        // State 5: the terminal's shell is open, which is one connection on the head unit.
        connector.agent.handlers["channel.get"] = { """{"switched":true,"on":true,"open":1}""" }
        terminal.value = TerminalState.Open("car")
        val open = viewModel.card("a shell open") { it.connections == 1 }
        assertEquals("1 connection open", open.switchLine)
        assertTrue(open.terminalOpen)
        assertTrue(open.canOpenTerminal)

        // Two, read again when the shell is held by a job, and the line says so.
        connector.agent.handlers["channel.get"] = { """{"switched":true,"on":true,"open":2}""" }
        terminal.value = TerminalState.Open("car", held = true)
        assertEquals("2 connections open", viewModel.card("two") { it.connections == 2 }.switchLine)

        // The shell ends: the row offers to open one again, and the count is read again.
        connector.agent.handlers["channel.get"] = { """{"switched":true,"on":true,"open":0}""" }
        terminal.value = TerminalState.Ended("car", TerminalEnd.Exited(0))
        val ended = viewModel.card("ended") { !it.terminalOpen && it.connections == 0 }
        assertEquals(SSH_OFF_AFTER_RESTART, ended.switchLine)
        assertTrue(ended.canOpenTerminal)
    }

    @Test
    fun `the terminal's row follows the terminal of this head unit only`() {
        val page = HeadUnitDetailUiState(link = LinkState.Connected("TCP"), channels = true, installed = listOf(InstalledPackage(SshTunnel.PACKAGE, "SSH access", "1.0.0")))
        val on = SshSwitchUi(on = true, connections = 0)
        fun row(state: TerminalState) = debugCard(page, false, on, state, "car")!!.terminalOpen
        assertFalse(row(TerminalState.Idle))
        assertTrue(row(TerminalState.Connecting("car")))
        assertTrue(row(TerminalState.Open("car")))
        assertTrue(row(TerminalState.Open("car", held = true)))
        assertFalse(row(TerminalState.Open("other")))
        assertFalse(row(TerminalState.Ended("car", TerminalEnd.ByUser)))
        assertFalse(row(TerminalState.Failed("car", "no")))
        assertFalse(row(TerminalState.Off("car")))
        // A terminal open can be gone back to whatever the switch is known to be.
        assertTrue(debugCard(page.copy(link = LinkState.Idle), false, SshSwitchUi(), TerminalState.Open("car"), "car")!!.canOpenTerminal)
    }

    @Test
    fun `with no link the switch is not known, so it is shown off and greyed, as is Open`() = test {
        agents()
        channel.on = true
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("on") { it.on == true }

        jobs.disconnect()
        // State 6: what the agent said last is not shown as how it stands now.
        val apart = viewModel.card("no link") { it.on == null }
        assertEquals(DebugCard(installed = true), apart)
        assertFalse(apart.canSwitch)
        assertFalse(apart.canOpenTerminal)
        viewModel.switchSsh(true)
        assertFalse(viewModel.uiState.value.confirmingSsh)
    }

    // ---- The switch ----

    @Test
    fun `switching on asks first, and Cancel sends nothing`() = test {
        agents()
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("off") { it.on == false }

        viewModel.switchSsh(true)
        await(viewModel.uiState, "the question") { it.confirmingSsh }
        viewModel.cancelSsh()
        await(viewModel.uiState, "the question put away") { !it.confirmingSsh }
        delay(200)
        assertEquals(0, requests("channel.set"))
        assertEquals(false, viewModel.uiState.value.debug?.on)
        assertFalse(channel.on)
    }

    @Test
    fun `Turn on switches it on, and switching off asks nothing`() = test {
        agents()
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("off") { it.on == false }

        viewModel.switchSsh(true)
        await(viewModel.uiState, "the question") { it.confirmingSsh }
        viewModel.confirmSsh()
        val on = viewModel.card("on") { it.on == true && !it.switching }
        assertFalse(viewModel.uiState.value.confirmingSsh)
        assertTrue(channel.on)
        assertNull(on.error)
        assertTrue(on.canOpenTerminal)

        viewModel.switchSsh(false)
        assertFalse(viewModel.uiState.value.confirmingSsh)
        viewModel.card("off again") { it.on == false && !it.switching }
        assertFalse(channel.on)
        assertEquals(2, requests("channel.set"))
    }

    @Test
    fun `a refusal is a line of its own, with the switch back at what the head unit has`() = test {
        agents()
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("off") { it.on == false }
        val set = requireNotNull(connector.agent.handlers["channel.set"])
        connector.agent.handlers["channel.set"] = { throw Refusal("busy") }

        viewModel.switchSsh(true)
        await(viewModel.uiState, "the question") { it.confirmingSsh }
        viewModel.confirmSsh()
        // State 7.
        val refused = viewModel.card("the refusal") { it.error != null && !it.switching }
        assertEquals("The head unit is busy with a job. Try again once it has finished.", refused.error)
        assertEquals(false, refused.on)
        assertTrue(refused.canSwitch)
        assertFalse(refused.canOpenTerminal)

        // The next throw puts the line away.
        connector.agent.handlers["channel.set"] = set
        viewModel.switchSsh(true)
        viewModel.confirmSsh()
        assertNull(viewModel.card("on") { it.on == true && !it.switching }.error)
    }

    @Test
    fun `the refusal's line stays through the reading right after it, and goes once the job ends and the switch is read`() = test {
        val planId = "0123456789abcdef"
        connector.onConnect = { agent ->
            agent.capabilities = """["channels"]"""
            agent.installed = sshInstalled
            agent.addChannel(channel)
            // A job of another controller's runs on the head unit as the app connects.
            agent.handlers["job.status"] = {
                """{"job":{"planId":"$planId","state":"installing","step":1,"steps":1,"id":"org.nbtapp.slow","seq":1},"events":[],"more":false}"""
            }
            agent.handlers["channel.set"] = { throw Refusal("busy") }
        }
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("off") { it.on == false }

        viewModel.switchSsh(true)
        await(viewModel.uiState, "the question") { it.confirmingSsh }
        viewModel.confirmSsh()
        // The switch is read again within the throw, which leaves the line.
        val refused = viewModel.card("the refusal") { it.error != null && !it.switching }
        assertEquals("The head unit is busy with a job. Try again once it has finished.", refused.error)
        val gets = requests("channel.get")
        delay(200)
        assertEquals(refused.error, viewModel.uiState.value.debug?.error)

        // The job ends: the switch is read, and the line goes.
        connector.agent.event(
            "job.done",
            2,
            """{"planId":"$planId","state":"succeeded","steps":[{"action":"install","id":"org.nbtapp.slow","version":"1.0.0","result":"done"}],"restart":"none"}""",
        )
        assertNull(viewModel.card("the line gone") { it.error == null }.error)
        assertTrue(requests("channel.get") > gets)
    }

    @Test
    fun `the refusal's line goes at the next throw`() = test {
        agents()
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("off") { it.on == false }
        val set = requireNotNull(connector.agent.handlers["channel.set"])
        connector.agent.handlers["channel.set"] = { throw Refusal("busy") }
        viewModel.switchSsh(true)
        viewModel.confirmSsh()
        viewModel.card("the refusal") { it.error != null && !it.switching }

        connector.agent.handlers["channel.set"] = set
        viewModel.switchSsh(true)
        viewModel.confirmSsh()
        // Gone as the throw begins, and not back once it is taken.
        assertNull(viewModel.card("on") { it.on == true && !it.switching }.error)
    }

    @Test
    fun `a switch that the head unit changed meanwhile is shown as it has it after a refusal`() = test {
        agents()
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("off") { it.on == false }
        // Switched on from the head unit's shell; the throw from here is refused.
        channel.on = true
        connector.agent.handlers["channel.set"] = { throw Refusal("io-error") }

        viewModel.switchSsh(false)
        val refused = viewModel.card("the refusal") { it.error != null && !it.switching }
        assertEquals("The head unit couldn't change the switch.", refused.error)
        assertEquals(true, refused.on)
    }

    @Test
    fun `the row SSH from other apps in its four forms`() {
        val off = DebugCard(installed = true, on = false)
        assertEquals("Switch on SSH access first", off.portLine)
        assertEquals("Set up", off.portButton)
        assertFalse(off.canOpenPort)
        // Not known, with no link: the same.
        assertEquals("Switch on SSH access first", DebugCard(installed = true).portLine)
        assertFalse(DebugCard(installed = true).canOpenPort)

        val ready = DebugCard(installed = true, on = true)
        assertEquals("For an SSH app on this phone, or a laptop", ready.portLine)
        assertEquals("Set up", ready.portButton)
        assertTrue(ready.canOpenPort)

        val port = SshPortState.On("car", 2222, connections = 1, hostKey = "SHA256:x")
        val on = DebugCard(installed = true, on = true, port = port)
        assertEquals("On · port 2222 · 1 connection", on.portLine)
        assertEquals("Manage", on.portButton)
        assertTrue(on.canOpenPort)

        val wide = DebugCard(installed = true, on = true, port = port.copy(otherDevices = true, connections = 2))
        assertEquals("On for other devices · port 2222 · 2 connections", wide.portLine)
        // While the port is on here, Manage is there whatever the switch is known to be.
        assertTrue(wide.copy(on = null).canOpenPort)
        assertTrue(DebugCard(installed = true, on = false, port = port).canOpenPort)
    }

    @Test
    fun `the row follows the port of this head unit only`() {
        val page = HeadUnitDetailUiState(link = LinkState.Connected("TCP"), channels = true, installed = listOf(InstalledPackage(SshTunnel.PACKAGE, "SSH access", "1.0.0")))
        val on = SshSwitchUi(on = true, connections = 0)
        val there = SshPortState.On("other", 2222, hostKey = "SHA256:x")
        assertEquals(SshPortState.Off(), debugCard(page, false, on, TerminalState.Idle, "car", there)!!.port)
        val here = SshPortState.On("car", 2223, hostKey = "SHA256:x")
        assertEquals(here, debugCard(page, false, on, TerminalState.Idle, "car", here)!!.port)
    }

    @Test
    fun `switching SSH access off from the card tells the port at once`() = test {
        agents()
        channel.on = true
        val told = java.util.concurrent.CopyOnWriteArrayList<Boolean>()
        val viewModel = mainDispatcherRule.track(
            HeadUnitDetailViewModel(
                "car", headUnits, jobs, connector, FakePhoneBluetooth(), setup, scope,
                terminal = terminal,
                catalogue = catalogue,
                sshSwitched = { told += it },
            ),
        )
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("on") { it.on == true }
        viewModel.switchSsh(false)
        viewModel.card("off") { it.on == false && !it.switching }
        assertEquals(listOf(false), told)
    }

    @Test
    fun `each refusal has its own line`() {
        fun line(code: String) = sshSwitchError(AgentRefusal(code, AgentText.request(code)))
        assertEquals("The head unit is busy with a job. Try again once it has finished.", line("busy"))
        assertEquals("SSH access isn't installed on the head unit any more.", line("not-found"))
        assertEquals("This head unit's agent can't switch SSH access. Update the agent.", line("unsupported"))
        assertEquals("The head unit couldn't change the switch.", line("io-error"))
        assertEquals(AgentText.request("forbidden"), line("forbidden"))
        assertEquals(AgentText.request("bad-request"), line("bad-request"))
        assertEquals("Not connected to a head unit", sshSwitchError(IOException("Not connected to a head unit")))
        assertEquals("The head unit didn't answer", sshSwitchError(IOException()))
    }

    @Test
    fun `the count of connections is said with its plural, and none or none known leaves the line as it is`() {
        assertNull(connectionsOpen(null))
        assertNull(connectionsOpen(0))
        assertEquals("1 connection open", connectionsOpen(1))
        assertEquals("3 connections open", connectionsOpen(3))
        assertEquals(SSH_OFF_AFTER_RESTART, DebugCard(installed = true, on = true, connections = null).switchLine)
    }

    @Test
    fun `with no link the card is there where the package was installed when the agent last said`() = test {
        headUnits.update("car") { it.copy(profile = HeadUnitProfile("1.7.0-beta.1", 1, FreeSpace(1, 1, 1, 1), RestartKind.NONE, listOf(InstalledPackage(SshTunnel.PACKAGE, "SSH access", "1.0.0")), 1L)) }
        val viewModel = viewModel()
        assertEquals(DebugCard(installed = true), viewModel.card("the card") { true })
    }
}

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
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.FakeChannel
import com.nbtapps.store.data.jobs.FakeConnector
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.Refusal
import com.nbtapps.store.data.packages.PackageDownloader
import com.nbtapps.store.data.packages.PackageStore
import com.nbtapps.store.data.ssh.SshAccess
import com.nbtapps.store.data.ssh.SshEntry
import com.nbtapps.store.data.ssh.SshFixtures.offering
import com.nbtapps.store.data.ssh.SshFixtures.sshVersion
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.ssh.SshSwitchState
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.ssh.sshAccessOver
import com.nbtapps.store.data.ssh.sshOffered
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.FreeSpace
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.withTestTimeout
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
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
 * The Debug card on the head unit's page: when it is there, its one row in each form, and its view model over the
 * fake agent, whose SSH package's channel stands in for the real one, with the app's SSH switch ([SshAccess]) over the
 * same link. The view model's tests wait for what they look for, never for a time.
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
    private val noted = CopyOnWriteArrayList<Pair<String, Boolean>>()
    private val access by lazy { sshAccessOver(jobs, scope, terminal = terminal, catalogue = catalogue, noted = { id, on -> noted += id to on }) }

    private val car = HeadUnit("car", "Test car", listOf(SerialEndpoint("00:11:22:33:44:55", "Test car")))

    /** The SSH package's channel on the fake agent: switched, off until switched on. */
    private val channel = FakeChannel(SshTunnel.PACKAGE, SshTunnel.CHANNEL, switched = true)

    private val sshInstalled = """[{"id":"${SshTunnel.PACKAGE}","name":"SSH access","version":"1.0.0"}]"""
    private val withSsh = listOf(InstalledPackage(SshTunnel.PACKAGE, "SSH access", "1.0.0"))

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

    private val refreshes = CopyOnWriteArrayList<String>()

    private fun viewModel() = mainDispatcherRule.track(
        HeadUnitDetailViewModel(
            "car", headUnits, jobs, connector, FakePhoneBluetooth(), setup, scope,
            catalogue = catalogue,
            sshEntry = access.entry,
            refreshSsh = { id ->
                refreshes += id
                access.refresh(id)
            },
        ),
    )

    private suspend fun <T> await(flow: StateFlow<T>, what: String, predicate: (T) -> Boolean): T =
        withTimeoutOrNull(10_000) { flow.first(predicate) } ?: throw AssertionError("never came: $what; last was ${flow.value}")

    private suspend fun HeadUnitDetailViewModel.card(what: String, predicate: (DebugCard) -> Boolean): DebugCard =
        await(uiState, what) { it.debug?.let(predicate) == true }.debug!!

    /** The requests of [method] the agent of the link has had. */
    private fun requests(method: String) = connector.agent.requests.count { it.first == method }

    // ---- When the card is there, and its row ----

    @Test
    fun `the card is there with a link to an agent that has channels and the package, or the catalogue's offer of it`() {
        val connected = HeadUnitDetailUiState(link = LinkState.Connected("TCP"), channels = true)
        val none = SshEntry.NotConnected

        assertNull(debugCard(connected, offered = false, none, "car"))
        assertEquals(DebugCard.Install, debugCard(connected, offered = true, none, "car"))
        assertEquals(DebugCard.Ready(null), debugCard(connected.copy(installed = withSsh), offered = false, none, "car"))
        // An agent without channels has no card, whatever is installed or offered.
        assertNull(debugCard(connected.copy(channels = false, installed = withSsh), offered = true, none, "car"))
        // With no link: only where the package was there when the agent last said, and it cannot be opened then.
        val apart = HeadUnitDetailUiState(link = LinkState.Idle, installed = withSsh)
        assertEquals(DebugCard.Apart, debugCard(apart, offered = false, SshEntry.Ready("car", "Test car", on = true), "car"))
        assertNull(debugCard(apart.copy(installed = emptyList()), offered = true, none, "car"))
        // A link being made is no link yet.
        assertEquals(DebugCard.Apart, debugCard(apart.copy(link = LinkState.Connecting), offered = false, none, "car"))
    }

    @Test
    fun `an offer that does not run on the agent is no offer, so there is no card`() {
        val connected = HeadUnitDetailUiState(link = LinkState.Connected("TCP"), channels = true, agentVersion = "1.0.0")
        val unrunnable = offered(offering(sshVersion(">=1.7.0-beta.1")), connected)
        assertNull(debugCard(connected, unrunnable, SshEntry.NotConnected, "car"))
        assertEquals(DebugCard.Install, debugCard(connected, offered(offering(sshVersion(">=1.0.0")), connected), SshEntry.NotConnected, "car"))
    }

    private fun offered(catalogue: CatalogueState, page: HeadUnitDetailUiState) =
        sshOffered(catalogue, page.installed, page.agentVersion, emptySet())

    @Test
    fun `the row says what the menu says of this head unit, and nothing of another's`() {
        val page = HeadUnitDetailUiState(link = LinkState.Connected("TCP"), channels = true, installed = withSsh)
        val port = SshPortState.On("car", 2222, hostKey = "SHA256:x")
        val busiest = SshEntry.Ready("car", "Test car", on = true, terminalOpen = true, port = port)
        assertEquals(DebugCard.Ready("On · terminal open · port 2222"), debugCard(page, false, busiest, "car"))
        assertEquals(DebugCard.Ready("Off"), debugCard(page, false, SshEntry.Ready("car", "Test car", on = false), "car"))
        assertEquals(DebugCard.Ready(null), debugCard(page, false, SshEntry.Ready("other", "Other car", on = true), "car"))
    }

    @Test
    fun `the row in its forms, open over the link and greyed and closed without one`() {
        assertEquals("Off", DebugCard.Ready("Off").rowLine)
        assertTrue(DebugCard.Ready("Off").canOpen)
        assertNull(DebugCard.Ready(null).rowLine)
        assertTrue(DebugCard.Ready(null).canOpen)
        assertEquals("Connect to the head unit to use SSH", DebugCard.Apart.rowLine)
        assertFalse(DebugCard.Apart.canOpen)
        assertFalse(DebugCard.Install.canOpen)
    }

    // ---- Over the fake agent ----

    @Test
    fun `an agent with channels and no package shows the card where the catalogue offers the package, with nothing read`() = test {
        agents(installed = "[]")
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        assertNull(await(viewModel.uiState, "the link") { it.link is LinkState.Connected && it.channels }.debug)

        catalogue.value = offering(sshVersion(">=1.0.0"))
        assertEquals(DebugCard.Install, viewModel.card("the offer") { true })
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
        assertEquals(0, requests("channel.get"))
    }

    @Test
    fun `the row follows the switch as the link reads it, on, and the connections open`() = test {
        agents()
        val viewModel = viewModel()
        assertEquals(listOf("car"), refreshes)
        assertTrue(jobs.connect("car").isSuccess)
        assertEquals(DebugCard.Ready("Off"), viewModel.card("off") { it == DebugCard.Ready("Off") })

        // Read again once the head unit has it on and the terminal changes.
        channel.on = true
        terminal.value = TerminalState.Connecting("car")
        assertEquals(DebugCard.Ready("On · terminal open"), viewModel.card("on") { it == DebugCard.Ready("On · terminal open") })
        connector.agent.handlers["channel.get"] = { """{"switched":true,"on":true,"open":1}""" }
        terminal.value = TerminalState.Open("car")
        await(access.state, "a shell open") { it.connections == 1 }
    }

    @Test
    fun `with no link the row is greyed and cannot be opened`() = test {
        agents()
        channel.on = true
        val viewModel = viewModel()
        assertTrue(jobs.connect("car").isSuccess)
        viewModel.card("on") { it == DebugCard.Ready("On") }

        jobs.disconnect()
        assertEquals(DebugCard.Apart, viewModel.card("no link") { it == DebugCard.Apart })
        assertEquals(SshSwitchState(), await(access.state, "forgotten") { it.headUnitId == null })
    }

    @Test
    fun `over the link the switch is thrown with channel set, a refusal is told, and switching off tells the port`() = test {
        agents()
        assertTrue(jobs.connect("car").isSuccess)
        await(access.state, "off") { it.on == false }
        val set = requireNotNull(connector.agent.handlers["channel.set"])

        connector.agent.handlers["channel.set"] = { throw Refusal("busy") }
        access.switch("car", true)
        access.confirm("car")
        val refused = await(access.state, "the refusal") { it.error != null && !it.switching }
        assertEquals("The head unit is busy with a job. Try again once it has finished.", refused.error)
        assertEquals(false, refused.on)

        connector.agent.handlers["channel.set"] = set
        access.switch("car", true)
        access.confirm("car")
        assertNull(await(access.state, "on") { it.on == true && !it.switching }.error)
        assertTrue(channel.on)
        access.switch("car", false)
        await(access.state, "off again") { it.on == false && !it.switching }
        assertFalse(channel.on)
        assertEquals("car" to false, noted.last())
    }

    @Test
    fun `with no link the card is there where the package was installed when the agent last said`() = test {
        headUnits.update("car") { it.copy(profile = HeadUnitProfile("1.7.0-beta.1", 1, FreeSpace(1, 1, 1, 1), RestartKind.NONE, withSsh, 1L)) }
        val viewModel = viewModel()
        assertEquals(DebugCard.Apart, viewModel.card("the card") { true })
    }
}

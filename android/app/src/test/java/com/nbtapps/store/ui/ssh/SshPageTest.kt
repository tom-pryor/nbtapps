// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.ssh

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.ssh.PortOffReason
import com.nbtapps.store.data.ssh.SshEntry
import com.nbtapps.store.data.ssh.SshFixtures.CAR
import com.nbtapps.store.data.ssh.SshFixtures.OTHER
import com.nbtapps.store.data.ssh.SshFixtures.connected
import com.nbtapps.store.data.ssh.SshInputs
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.ssh.SshSwitchState
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.FreeSpace
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.HeadUnitProfile
import com.nbtapps.store.model.InstalledPackage
import com.nbtapps.store.model.RestartKind
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.ui.headunits.ConnectGuard
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/**
 * The SSH page: what it shows over the link to its head unit and without one ([sshPage]), its card's rows, and its
 * view model over the app's SSH switch with stand-ins, in virtual time: the switch and its question, the link lost
 * while the page is open, and Connect from the banner, through the question before a connect.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class SshPageTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    private val sshProfile = HeadUnitProfile(
        "1.7.0",
        1,
        FreeSpace(1, 1, 1, 1),
        RestartKind.NONE,
        listOf(InstalledPackage(SshTunnel.PACKAGE, "SSH access", "1.0.0")),
        1L,
    )
    private val car = HeadUnit(CAR, "Test car", listOf(SerialEndpoint("00:11:22:33:44:55", "Test car")), profile = sshProfile)

    private fun page(
        connection: ConnectionState = connected(CAR),
        entry: SshEntry = SshEntry.Ready(CAR, "Test car", on = false),
        switch: SshSwitchState = SshSwitchState(CAR, on = false, connections = 0),
        terminal: TerminalState = TerminalState.Idle,
        port: SshPortState = SshPortState.Off(),
        unit: HeadUnit? = car,
    ) = sshPage(CAR, loaded = true, unit, connection, entry, switch, terminal, port)

    private fun SshUiState.card(): SshCard = (content as SshPageContent.Controls).card

    // ---- What it shows ----

    @Test
    fun `before the head units are read it waits, and a head unit no longer saved is not found`() {
        assertEquals(SshUiState(), sshPage(CAR, loaded = false, null, connected(CAR), SshEntry.NotConnected, SshSwitchState(), TerminalState.Idle, SshPortState.Off()))
        assertEquals(SshPageContent.NotFound, page(unit = null).content)
    }

    @Test
    fun `over the link it is the card, with the switch, its count, a throw and its refusal as the app has them`() {
        val state = page(
            entry = SshEntry.Ready(CAR, "Test car", on = true, terminalOpen = true),
            switch = SshSwitchState(CAR, on = true, connections = 1, switching = true, error = "No", confirming = true),
        )
        assertEquals("Test car", state.name)
        assertFalse(state.apart)
        assertTrue(state.confirming)
        assertEquals(SshCard(on = true, connections = 1, switching = true, error = "No", terminalOpen = true), state.card())
    }

    @Test
    fun `over the link without the package it offers the install, and says so where the agent cannot run it`() {
        assertEquals(SshPageContent.Install, page(connected(CAR, ssh = null), SshEntry.NotInstalled(CAR, "Test car"), SshSwitchState()).content)
        assertEquals(SshPageContent.CantRun, page(connected(CAR, channels = false), SshEntry.CantRun(CAR, "Test car"), SshSwitchState()).content)
        assertFalse(page(connected(CAR, ssh = null), SshEntry.NotInstalled(CAR, "Test car"), SshSwitchState()).apart)
    }

    @Test
    fun `with the link lost the banner is up, and the switch is off and greyed with its count, throw and question gone`() {
        val lost = page(
            ConnectionState.Failed(CAR, "The link was lost"),
            SshEntry.NotConnected,
            SshSwitchState(),
            terminal = TerminalState.Ended(CAR, TerminalEnd.LinkLost("The link was lost")),
            port = SshPortState.Off(CAR, reason = PortOffReason.LINK_LOST),
        )
        assertTrue(lost.apart)
        assertFalse(lost.connecting)
        assertFalse(lost.confirming)
        val card = lost.card()
        assertEquals(SshCard(port = SshPortState.Off(CAR, reason = PortOffReason.LINK_LOST)), card)
        assertFalse(card.canSwitch)
        // The terminal ended with the link: it is opened again, not resumed, once SSH access is known to be on.
        assertFalse(card.terminalOpen)
        assertFalse(card.canOpenTerminal)
        // The port went off with the link.
        assertEquals("Switch on SSH access first", card.portLine)
        assertFalse(card.canOpenPort)
    }

    @Test
    fun `with another head unit connected it is apart too, and nothing of the other's is taken`() {
        val elsewhere = page(
            connected(OTHER),
            SshEntry.Ready(OTHER, "Other car", on = true),
            SshSwitchState(OTHER, on = true, connections = 2, confirming = true),
            terminal = TerminalState.Open(OTHER),
            port = SshPortState.On(OTHER, 2222, hostKey = "SHA256:x"),
        )
        assertTrue(elsewhere.apart)
        assertFalse(elsewhere.confirming)
        assertEquals(SshCard(), elsewhere.card())
    }

    @Test
    fun `apart, the rows follow the terminal and the port as they are for this head unit`() {
        val port = SshPortState.On(CAR, 2222, hostKey = "SHA256:x")
        val card = page(ConnectionState.Disconnected, SshEntry.NotConnected, SshSwitchState(), TerminalState.Open(CAR, held = true), port).card()
        assertTrue(card.terminalOpen)
        assertTrue(card.canOpenTerminal)
        assertEquals("Manage", card.portButton)
        assertTrue(card.canOpenPort)
    }

    @Test
    fun `apart, a head unit whose package was not there when last seen has the install row, and a connect under way is said`() {
        val none = car.copy(profile = sshProfile.copy(installed = emptyList()))
        assertEquals(SshPageContent.Install, page(ConnectionState.Disconnected, SshEntry.NotConnected, SshSwitchState(), unit = none).content)
        val connecting = page(ConnectionState.Connecting(CAR), SshEntry.NotConnected, SshSwitchState())
        assertTrue(connecting.apart)
        assertTrue(connecting.connecting)
        assertFalse(page(ConnectionState.Connecting(OTHER), SshEntry.NotConnected, SshSwitchState()).connecting)
    }

    @Test
    fun `the card's rows in their forms`() {
        val off = SshCard(on = false)
        assertEquals(SSH_OFF_AFTER_RESTART, off.switchLine)
        assertTrue(off.canSwitch)
        assertFalse(off.canOpenTerminal)
        assertEquals("Switch on SSH access first", off.portLine)
        assertEquals("Set up", off.portButton)
        assertFalse(off.canOpenPort)

        val ready = SshCard(on = true)
        assertTrue(ready.canOpenTerminal)
        assertEquals("For an SSH app on this phone, or a laptop", ready.portLine)
        assertTrue(ready.canOpenPort)
        assertFalse(ready.copy(switching = true).canSwitch)

        val port = SshPortState.On(CAR, 2222, connections = 1, hostKey = "SHA256:x")
        assertEquals("On · port 2222 · 1 connection", ready.copy(port = port).portLine)
        assertEquals("Manage", ready.copy(port = port).portButton)
        assertEquals("On for other devices · port 2222 · 2 connections", ready.copy(port = port.copy(otherDevices = true, connections = 2)).portLine)
        assertEquals("Checking the way to the head unit…", ready.copy(port = SshPortState.Checking(CAR)).portLine)

        assertEquals("1 connection open", SshCard(on = true, connections = 1).switchLine)
        assertEquals("3 connections open", SshCard(on = true, connections = 3).switchLine)
        assertEquals(SSH_OFF_AFTER_RESTART, SshCard(on = true, connections = 0).switchLine)
    }

    // ---- The view model ----

    private val inputs = SshInputs()
    private val unit = MutableStateFlow<HeadUnit?>(car)
    private val connects = mutableListOf<String>()

    /** The page's view model over [inputs], whose connect sets the link up at once; [guard] stands before it. */
    private fun TestScope.viewModel(guard: ConnectGuard = ConnectGuard.NONE): SshViewModel {
        val access = inputs.access(backgroundScope)
        runCurrent()
        val viewModel = mainDispatcherRule.track(
            SshViewModel(CAR, unit, inputs.connection, access, inputs.terminal, inputs.port, guard) { id ->
                connects += id
                inputs.connection.value = connected(id)
            },
        )
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.uiState.collect {} }
        runCurrent()
        return viewModel
    }

    @Test
    fun `the page reads the switch again as it opens`() = runTest {
        inputs.connection.value = connected(CAR)
        val viewModel = viewModel()
        // Once for the link, once for the page.
        assertEquals(listOf(CAR, CAR), inputs.switch.reads)
        assertEquals(SshCard(on = false, connections = 0), viewModel.uiState.value.card())
    }

    @Test
    fun `the switch asks first, then Turn on switches it on`() = runTest {
        inputs.connection.value = connected(CAR)
        val viewModel = viewModel()
        viewModel.switch(true)
        assertTrue(viewModel.uiState.value.confirming)
        viewModel.cancel()
        assertFalse(viewModel.uiState.value.confirming)
        viewModel.switch(true)
        viewModel.confirm()
        runCurrent()
        assertFalse(viewModel.uiState.value.confirming)
        assertEquals(true, viewModel.uiState.value.card().on)
        assertEquals(listOf(CAR to true), inputs.switch.writes)
    }

    @Test
    fun `the link lost while the page is open puts the banner up, and Connect brings the switch back`() = runTest {
        inputs.switch.on = true
        inputs.connection.value = connected(CAR)
        val viewModel = viewModel()
        assertEquals(true, viewModel.uiState.value.card().on)

        inputs.connection.value = ConnectionState.Failed(CAR, "The link was lost")
        inputs.terminal.value = TerminalState.Ended(CAR, TerminalEnd.LinkLost("The link was lost"))
        runCurrent()
        val lost = viewModel.uiState.value
        assertTrue(lost.apart)
        assertEquals(SshCard(), lost.card())

        viewModel.connect()
        runCurrent()
        assertEquals(listOf(CAR), connects)
        val back = viewModel.uiState.value
        assertFalse(back.apart)
        assertEquals(SshCard(on = true, connections = 0), back.card())
    }

    @Test
    fun `Connect goes through the question first, and Cancel connects nothing where Connect connects`() = runTest {
        val answers = ArrayDeque<CompletableDeferred<Boolean>>()
        val asked = mutableListOf<String>()
        val guard = ConnectGuard { id ->
            asked += id
            CompletableDeferred<Boolean>().also { answers.addLast(it) }.await()
        }
        inputs.connection.value = connected(OTHER)
        val viewModel = viewModel(guard)
        assertTrue(viewModel.uiState.value.apart)

        viewModel.connect()
        runCurrent()
        assertEquals(listOf(CAR), asked)
        answers.removeFirst().complete(false)
        runCurrent()
        assertEquals(emptyList<String>(), connects)
        assertTrue(viewModel.uiState.value.apart)

        viewModel.connect()
        runCurrent()
        answers.removeFirst().complete(true)
        runCurrent()
        assertEquals(listOf(CAR), connects)
        assertFalse(viewModel.uiState.value.apart)
        assertEquals(false, viewModel.uiState.value.card().on)
    }

    @Test
    fun `the package removed while the page is open turns the card into the install's row`() = runTest {
        inputs.connection.value = connected(CAR)
        val viewModel = viewModel()
        inputs.connection.value = connected(CAR, ssh = null)
        runCurrent()
        assertEquals(SshPageContent.Install, viewModel.uiState.value.content)
        assertFalse(viewModel.uiState.value.apart)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.menu

import androidx.compose.material3.DrawerValue.Closed
import androidx.compose.material3.DrawerValue.Open
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.ssh.SshEntry
import com.nbtapps.store.data.ssh.SshFixtures
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.ssh.SshTunnel
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.TcpEndpoint
import com.nbtapps.store.ui.restart.RestartQuestions
import com.nbtapps.store.ui.restart.RestartStep
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

/**
 * When the side menu counts as in sight, and when it has to close for the screen on top; its section for the head unit
 * the app is connected to, with the SSH entry and Restart iDrive in each state, where a tap on them goes, and its view
 * model.
 */
class SideMenuTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    @Test
    fun `the menu shows from the tap that opens it until it has gone`() {
        assertFalse(menuShows(current = Closed, target = Closed))
        assertTrue("on its way in", menuShows(current = Closed, target = Open))
        assertTrue(menuShows(current = Open, target = Open))
        assertTrue("on its way out", menuShows(current = Open, target = Closed))
    }

    @Test
    fun `the menu gives way to a screen that is not a tab, and stays over a tab`() {
        assertTrue(menuGivesWay(screenKnown = true, tabOnTop = false))
        assertFalse(menuGivesWay(screenKnown = true, tabOnTop = true))
    }

    @Test
    fun `the menu stays as it is while the screen on top is not known, as just after a rotation`() {
        assertFalse(menuGivesWay(screenKnown = false, tabOnTop = false))
    }

    // ---- The section of the head unit connected ----

    @Test
    fun `with no head unit connected the entry is greyed, says to connect, and opens the Head units tab`() {
        assertEquals(
            SshMenuItem("No head unit connected", "Connect to a head unit first", greyed = true, install = false, dot = false, SshMenuTarget.HeadUnits),
            sshMenuItem(SshEntry.NotConnected),
        )
    }

    @Test
    fun `without the package the entry is greyed, offers the install, and opens the package's page`() {
        assertEquals(
            SshMenuItem(
                "Test car",
                "SSH access isn't installed on this head unit",
                greyed = true,
                install = true,
                dot = false,
                SshMenuTarget.Package(SshTunnel.PACKAGE),
            ),
            sshMenuItem(SshEntry.NotInstalled("car", "Test car")),
        )
    }

    @Test
    fun `where the agent cannot run it the entry is greyed, says to update the agent, and opens the head unit's page`() {
        assertEquals(
            SshMenuItem(
                "Test car",
                "This head unit's agent can't run SSH access. Update the agent.",
                greyed = true,
                install = false,
                dot = false,
                SshMenuTarget.HeadUnit("car"),
            ),
            sshMenuItem(SshEntry.CantRun("car", "Test car")),
        )
    }

    @Test
    fun `ready, the entry opens the SSH page, says how SSH stands, and has the dot while it is on`() {
        val ready = SshEntry.Ready("car", "Test car")
        val ssh = SshMenuTarget.Ssh("car")
        assertEquals(SshMenuItem("Test car", null, greyed = false, install = false, dot = false, ssh), sshMenuItem(ready))
        assertEquals(SshMenuItem("Test car", "Off", greyed = false, install = false, dot = false, ssh), sshMenuItem(ready.copy(on = false)))
        assertEquals(SshMenuItem("Test car", "On", greyed = false, install = false, dot = true, ssh), sshMenuItem(ready.copy(on = true)))
        val busiest = ready.copy(on = true, terminalOpen = true, port = SshPortState.On("car", 2222, hostKey = "SHA256:x"))
        assertEquals(
            SshMenuItem("Test car", "On · terminal open · port 2222", greyed = false, install = false, dot = true, ssh),
            sshMenuItem(busiest),
        )
    }

    // ---- Restart iDrive ----

    @Test
    fun `with no link up, Restart iDrive is greyed, says to connect first, and opens the Head units tab`() {
        val notConnected = listOf(
            ConnectionState.Disconnected,
            ConnectionState.Connecting("car"),
            ConnectionState.NeedsPairing("car", TcpEndpoint("192.0.2.1", 7144), stale = false),
            ConnectionState.Failed("car", "The car didn't connect to this app."),
        )
        notConnected.forEach { connection ->
            val item = restartMenuItem(connection)
            assertEquals("$connection", RestartMenuItem(null), item)
            assertTrue(item.greyed)
            assertEquals("Connect to a head unit first", item.line)
        }
    }

    @Test
    fun `with a link up, Restart iDrive asks about that head unit, with or without SSH there`() {
        val withSsh = SshFixtures.connected("car")
        val noPackage = SshFixtures.connected("car", ssh = null)
        val noChannels = SshFixtures.connected("car", channels = false)
        listOf(withSsh, noPackage, noChannels).forEach { connection ->
            val item = restartMenuItem(connection)
            assertEquals(RestartMenuItem("car"), item)
            assertFalse(item.greyed)
            assertEquals(null, item.line)
        }
    }

    private val connection = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)

    private fun viewModel(entry: MutableStateFlow<SshEntry> = MutableStateFlow(SshEntry.NotConnected)) = mainDispatcherRule.track(
        SideMenuViewModel(
            entry,
            connection,
            RestartQuestions(connection, MutableStateFlow(TerminalState.Idle), MutableStateFlow(SshPortState.Off()), { Result.success(Unit) }),
        ),
    )

    @Test
    fun `the view model's Restart iDrive follows the link, and its tap asks the question or says to open Head units`() = runTest {
        val viewModel = viewModel()
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.restartItem.collect {} }
        assertEquals(RestartMenuItem(null), viewModel.restartItem.value)
        assertFalse("greyed: Head units opens", viewModel.tapRestart(viewModel.restartItem.value))
        assertEquals(RestartStep.Closed, viewModel.restart.state.value)

        connection.value = SshFixtures.connected("car", ssh = null)
        assertEquals(RestartMenuItem("car"), viewModel.restartItem.value)
        assertTrue(viewModel.tapRestart(viewModel.restartItem.value))
        assertEquals(RestartStep.Asking("car", terminalOpen = false, portOpen = false), viewModel.restart.state.value)
        assertTrue("a second tap leaves the one question", viewModel.tapRestart(viewModel.restartItem.value))
        assertEquals(RestartStep.Asking("car", terminalOpen = false, portOpen = false), viewModel.restart.state.value)
    }

    @Test
    fun `the view model follows the entry`() = runTest {
        val entry = MutableStateFlow<SshEntry>(SshEntry.NotConnected)
        val viewModel = viewModel(entry)
        assertEquals(sshMenuItem(SshEntry.NotConnected), viewModel.ssh.value)
        backgroundScope.launch(mainDispatcherRule.testDispatcher) { viewModel.ssh.collect {} }
        entry.value = SshEntry.Ready("car", "Test car", on = true)
        assertEquals("On", viewModel.ssh.value.line)
        entry.value = SshEntry.NotConnected
        assertEquals(SshMenuTarget.HeadUnits, viewModel.ssh.value.target)
    }
}

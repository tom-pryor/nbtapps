// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.ssh.SshFixtures.CAR
import com.nbtapps.store.data.ssh.SshFixtures.NAMES
import com.nbtapps.store.data.ssh.SshFixtures.OTHER
import com.nbtapps.store.data.ssh.SshFixtures.connected
import com.nbtapps.store.data.ssh.SshFixtures.offering
import com.nbtapps.store.data.ssh.SshFixtures.sshVersion
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.TcpEndpoint
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** The menu's entry for SSH in each of its states ([sshEntry]), and the line it shows when it is ready. */
class SshEntryTest {

    private fun entry(
        connection: ConnectionState,
        catalogue: CatalogueState = offering(sshVersion()),
        switch: SshSwitchState = SshSwitchState(),
        terminal: TerminalState = TerminalState.Idle,
        port: SshPortState = SshPortState.Off(),
        names: Map<String, String> = NAMES,
    ) = sshEntry(connection, names, catalogue, emptySet(), switch, terminal, port)

    @Test
    fun `with no link up it is not connected, for none, one being made, one waiting for pairing, or one that was lost`() {
        val tcp = TcpEndpoint("192.0.2.1", 7144)
        assertEquals(SshEntry.NotConnected, entry(ConnectionState.Disconnected))
        assertEquals(SshEntry.NotConnected, entry(ConnectionState.Connecting(CAR)))
        assertEquals(SshEntry.NotConnected, entry(ConnectionState.NeedsPairing(CAR, tcp, stale = false)))
        assertEquals(SshEntry.NotConnected, entry(ConnectionState.Failed(CAR, "The link was lost")))
    }

    @Test
    fun `an agent without channels cannot run it, whatever is installed or offered`() {
        assertEquals(SshEntry.CantRun(CAR, "Test car"), entry(connected(CAR, channels = false)))
        assertEquals(SshEntry.CantRun(CAR, "Test car"), entry(connected(CAR, ssh = null, channels = false)))
    }

    @Test
    fun `without the package it is to be installed where the catalogue offers it, or has not been read yet`() {
        assertEquals(SshEntry.NotInstalled(CAR, "Test car"), entry(connected(CAR, ssh = null)))
        assertEquals(SshEntry.NotInstalled(CAR, "Test car"), entry(connected(CAR, ssh = null), catalogue = CatalogueState()))
        // A catalogue that could not be read has no index either.
        assertEquals(SshEntry.NotInstalled(CAR, "Test car"), entry(connected(CAR, ssh = null), catalogue = CatalogueState(error = "No network")))
    }

    @Test
    fun `without the package it cannot run where the catalogue has none that runs on the agent`() {
        assertEquals(SshEntry.CantRun(CAR, "Test car"), entry(connected(CAR, ssh = null), catalogue = offering(sshVersion(">=2.0.0"))))
        assertEquals(SshEntry.CantRun(CAR, "Test car"), entry(connected(CAR, ssh = null), catalogue = offering()))
    }

    @Test
    fun `with the package it is ready, with the switch as the agent said and the terminal and port of that head unit`() {
        assertEquals(SshEntry.Ready(CAR, "Test car"), entry(connected(CAR)))
        assertEquals(SshEntry.Ready(CAR, "Test car", on = true), entry(connected(CAR), switch = SshSwitchState(CAR, on = true)))
        // The catalogue has nothing to say once the package is there.
        assertEquals(SshEntry.Ready(CAR, "Test car", on = false), entry(connected(CAR), CatalogueState(), SshSwitchState(CAR, on = false)))

        val port = SshPortState.On(CAR, 2222, hostKey = "SHA256:x")
        assertEquals(
            SshEntry.Ready(CAR, "Test car", on = true, terminalOpen = true, port = port),
            entry(connected(CAR), switch = SshSwitchState(CAR, on = true), terminal = TerminalState.Open(CAR), port = port),
        )
        assertEquals(true, (entry(connected(CAR), terminal = TerminalState.Connecting(CAR)) as SshEntry.Ready).terminalOpen)
        assertEquals(false, (entry(connected(CAR), terminal = TerminalState.Ended(CAR, TerminalEnd.ByUser)) as SshEntry.Ready).terminalOpen)
    }

    @Test
    fun `with another head unit connected it is that one's, and nothing of the first is taken`() {
        val other = entry(
            connected(OTHER),
            switch = SshSwitchState(CAR, on = true),
            terminal = TerminalState.Open(CAR),
            port = SshPortState.On(CAR, 2222, hostKey = "SHA256:x"),
        )
        assertEquals(SshEntry.Ready(OTHER, "Other car"), other)
        assertEquals(SshEntry.NotInstalled(OTHER, "Other car"), entry(connected(OTHER, ssh = null)))
    }

    @Test
    fun `a head unit the saved ones do not name is called a head unit`() {
        assertEquals(SshEntry.Ready(CAR, "Head unit"), entry(connected(CAR), names = emptyMap()))
    }

    @Test
    fun `the line is made of what is open, and there is none until the switch is known`() {
        val ready = SshEntry.Ready(CAR, "Test car")
        assertNull(ready.line)
        assertNull(ready.copy(terminalOpen = true).line)
        assertEquals("Off", ready.copy(on = false).line)
        assertEquals("On", ready.copy(on = true).line)
        assertEquals("On · terminal open", ready.copy(on = true, terminalOpen = true).line)
        val port = SshPortState.On(CAR, 2222, hostKey = "SHA256:x")
        assertEquals("On · port 2222", ready.copy(on = true, port = port).line)
        assertEquals("On · terminal open · port 2222", ready.copy(on = true, terminalOpen = true, port = port).line)
        // A port still being checked has no number yet.
        assertEquals("On", ready.copy(on = true, port = SshPortState.Checking(CAR)).line)
    }
}

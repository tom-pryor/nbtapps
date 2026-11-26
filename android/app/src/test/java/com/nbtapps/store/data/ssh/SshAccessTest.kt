// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.ssh.SshFixtures.CAR
import com.nbtapps.store.data.ssh.SshFixtures.OTHER
import com.nbtapps.store.data.ssh.SshFixtures.connected
import com.nbtapps.store.data.ssh.SshFixtures.offering
import com.nbtapps.store.data.ssh.SshFixtures.sshVersion
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.AgentText
import java.io.IOException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The app's SSH switch ([SshAccess]) over stand-in inputs and a stand-in agent, in virtual time: when it is read and
 * when not, what is forgotten with the link, the throw and its question, each refusal's line and how long it stays,
 * and the order of a reading and a throw, both ways round. Every step runs what is due ([runCurrent]) before the
 * test looks; gates hold an answer back where the order matters.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class SshAccessTest {

    private val inputs = SshInputs()
    private val switch = inputs.switch

    private fun test(block: suspend TestScope.(SshAccess) -> Unit) = runTest {
        val access = inputs.access(backgroundScope)
        runCurrent()
        block(access)
    }

    // ---- When it is read ----

    @Test
    fun `the switch is read once as a link comes up to a head unit with the package and an agent with channels`() = test { access ->
        assertEquals(SshSwitchState(), access.state.value)
        inputs.connection.value = connected(CAR)
        runCurrent()
        assertEquals(listOf(CAR), switch.reads)
        assertEquals(SshSwitchState(CAR, on = false, connections = 0), access.state.value)
    }

    @Test
    fun `nothing is read from a head unit without the package, or from an agent without channels`() = test { access ->
        inputs.connection.value = connected(CAR, ssh = null)
        runCurrent()
        inputs.connection.value = connected(CAR, channels = false)
        runCurrent()
        // Nor for a page that opens meanwhile.
        access.refresh(CAR)
        runCurrent()
        assertEquals(emptyList<String>(), switch.reads)
        assertEquals(SshSwitchState(), access.state.value)
    }

    @Test
    fun `it is read again when the package's version, a job, the terminal or the port changes on that head unit`() = test { access ->
        inputs.connection.value = connected(CAR)
        runCurrent()
        assertEquals(1, switch.reads.size)

        inputs.connection.value = connected(CAR, ssh = "1.1.0")
        runCurrent()
        assertEquals(2, switch.reads.size)
        inputs.jobOn.value = CAR
        runCurrent()
        assertEquals(3, switch.reads.size)
        inputs.jobOn.value = null
        runCurrent()
        assertEquals(4, switch.reads.size)
        inputs.terminal.value = TerminalState.Open(CAR)
        runCurrent()
        assertEquals(5, switch.reads.size)
        inputs.port.value = SshPortState.On(CAR, 2222, hostKey = "SHA256:x")
        runCurrent()
        assertEquals(6, switch.reads.size)
        assertEquals(CAR, access.state.value.headUnitId)
    }

    @Test
    fun `another head unit's job, terminal or port does not read it again`() = test {
        inputs.connection.value = connected(CAR)
        runCurrent()
        inputs.jobOn.value = OTHER
        inputs.terminal.value = TerminalState.Open(OTHER)
        inputs.port.value = SshPortState.On(OTHER, 2222, hostKey = "SHA256:x")
        runCurrent()
        assertEquals(listOf(CAR), switch.reads)
    }

    @Test
    fun `a page that opens reads it again, only where the app is connected to its head unit`() = test { access ->
        access.refresh(CAR)
        runCurrent()
        assertEquals(emptyList<String>(), switch.reads)

        inputs.connection.value = connected(CAR)
        runCurrent()
        access.refresh(OTHER)
        runCurrent()
        assertEquals(listOf(CAR), switch.reads)
        access.refresh(CAR)
        runCurrent()
        assertEquals(listOf(CAR, CAR), switch.reads)
    }

    @Test
    fun `with the link lost what was said is forgotten, and another head unit's link starts afresh`() = test { access ->
        switch.on = true
        switch.open = 1
        inputs.connection.value = connected(CAR)
        runCurrent()
        assertEquals(SshSwitchState(CAR, on = true, connections = 1), access.state.value)

        inputs.connection.value = ConnectionState.Failed(CAR, "The link was lost")
        runCurrent()
        assertEquals(SshSwitchState(), access.state.value)

        // The other head unit's answer is held back: until it comes, nothing of the first is shown as the other's.
        switch.on = false
        switch.open = 0
        val gate = CompletableDeferred<Unit>().also { switch.readGate = it }
        inputs.connection.value = connected(OTHER)
        runCurrent()
        assertEquals(SshSwitchState(OTHER), access.state.value)
        gate.complete(Unit)
        runCurrent()
        assertEquals(SshSwitchState(OTHER, on = false, connections = 0), access.state.value)
    }

    @Test
    fun `a reading that fails leaves what the agent said before`() = test { access ->
        switch.on = true
        inputs.connection.value = connected(CAR)
        runCurrent()
        switch.readFailure = IOException("The head unit didn't answer")
        inputs.terminal.value = TerminalState.Open(CAR)
        runCurrent()
        assertEquals(2, switch.reads.size)
        assertEquals(true, access.state.value.on)
    }

    // ---- The throw ----

    @Test
    fun `switching on asks first, and Cancel sends nothing`() = test { access ->
        inputs.connection.value = connected(CAR)
        runCurrent()
        access.switch(CAR, true)
        assertTrue(access.state.value.confirming)
        access.cancel()
        runCurrent()
        assertFalse(access.state.value.confirming)
        assertEquals(emptyList<Pair<String, Boolean>>(), switch.writes)
        assertEquals(false, access.state.value.on)
    }

    @Test
    fun `Turn on switches it on, and switching off asks nothing and tells the port`() = test { access ->
        inputs.connection.value = connected(CAR)
        runCurrent()
        access.switch(CAR, true)
        access.confirm(CAR)
        assertFalse(access.state.value.confirming)
        assertTrue(access.state.value.switching)
        runCurrent()
        assertEquals(SshSwitchState(CAR, on = true, connections = 0), access.state.value)

        access.switch(CAR, false)
        assertFalse(access.state.value.confirming)
        runCurrent()
        assertEquals(SshSwitchState(CAR, on = false, connections = 0), access.state.value)
        assertEquals(listOf(CAR to true, CAR to false), switch.writes)
        assertEquals(CAR to false, switch.notes.last())
    }

    @Test
    fun `nothing is thrown while the switch is not known, while a throw waits, or for another head unit`() = test { access ->
        val read = CompletableDeferred<Unit>().also { switch.readGate = it }
        inputs.connection.value = connected(CAR)
        runCurrent()
        access.switch(CAR, false)
        access.switch(CAR, true)
        assertFalse(access.state.value.confirming)
        read.complete(Unit)
        runCurrent()

        access.switch(OTHER, true)
        assertFalse(access.state.value.confirming)
        access.confirm(OTHER)

        val write = CompletableDeferred<Unit>().also { switch.writeGate = it }
        access.switch(CAR, true)
        access.confirm(CAR)
        runCurrent()
        access.switch(CAR, false)
        assertFalse(access.state.value.confirming)
        write.complete(Unit)
        runCurrent()
        assertEquals(listOf(CAR to true), switch.writes)
        assertEquals(true, access.state.value.on)
    }

    @Test
    fun `a refusal is a line of its own, with the switch back at what the head unit has`() = test { access ->
        inputs.connection.value = connected(CAR)
        runCurrent()
        // Switched on from the head unit's own shell meanwhile; the throw from here is refused.
        switch.on = true
        switch.writeFailure = AgentRefusal("io-error", AgentText.request("io-error"))
        access.switch(CAR, false)
        runCurrent()
        val refused = access.state.value
        assertEquals("The head unit couldn't change the switch.", refused.error)
        assertEquals(true, refused.on)
        assertFalse(refused.switching)
        assertTrue(refused.canSwitch)
        assertEquals(2, switch.reads.size)
        assertEquals(emptyList<Pair<String, Boolean>>(), switch.notes)
    }

    @Test
    fun `the refusal's line stays through the reading within the throw, and goes at the next reading that follows the link`() = test { access ->
        inputs.jobOn.value = CAR
        inputs.connection.value = connected(CAR)
        runCurrent()
        switch.writeFailure = AgentRefusal("busy", AgentText.request("busy"))
        access.switch(CAR, true)
        access.confirm(CAR)
        runCurrent()
        assertEquals("The head unit is busy with a job. Try again once it has finished.", access.state.value.error)
        assertEquals(2, switch.reads.size)

        // The job ends: the switch is read, and the line goes.
        inputs.jobOn.value = null
        runCurrent()
        assertEquals(3, switch.reads.size)
        assertNull(access.state.value.error)
    }

    @Test
    fun `the refusal's line goes as the next throw begins`() = test { access ->
        inputs.connection.value = connected(CAR)
        runCurrent()
        switch.writeFailure = AgentRefusal("busy", AgentText.request("busy"))
        access.switch(CAR, false)
        runCurrent()
        assertTrue(access.state.value.error != null)

        switch.writeFailure = null
        access.switch(CAR, false)
        assertNull(access.state.value.error)
        runCurrent()
        assertNull(access.state.value.error)
    }

    @Test
    fun `an answer to a throw that comes after the link has gone is dropped`() = test { access ->
        switch.on = true
        inputs.connection.value = connected(CAR)
        runCurrent()
        val write = CompletableDeferred<Unit>().also { switch.writeGate = it }
        access.switch(CAR, false)
        runCurrent()
        inputs.connection.value = ConnectionState.Disconnected
        runCurrent()
        write.complete(Unit)
        runCurrent()
        assertEquals(SshSwitchState(), access.state.value)
    }

    @Test
    fun `a reading under way when the switch is thrown never takes the place of the throw's answer`() = test { access ->
        inputs.connection.value = connected(CAR)
        runCurrent()
        // The reading goes out first, and is answered after the throw has been asked for.
        val read = CompletableDeferred<Unit>().also { switch.readGate = it }
        inputs.terminal.value = TerminalState.Ended(CAR, TerminalEnd.ByUser)
        runCurrent()
        assertEquals(2, switch.reads.size)
        access.switch(CAR, true)
        access.confirm(CAR)
        runCurrent()
        // The throw waits for the reading.
        assertEquals(emptyList<Pair<String, Boolean>>(), switch.writes)
        read.complete(Unit)
        runCurrent()
        assertEquals(listOf(CAR to true), switch.writes)
        assertEquals(true, access.state.value.on)
        assertFalse(access.state.value.switching)
    }

    @Test
    fun `a reading asked for while a throw waits for the agent goes out after its answer`() = test { access ->
        inputs.connection.value = connected(CAR)
        runCurrent()
        val write = CompletableDeferred<Unit>().also { switch.writeGate = it }
        access.switch(CAR, true)
        access.confirm(CAR)
        runCurrent()
        inputs.terminal.value = TerminalState.Connecting(CAR)
        runCurrent()
        assertEquals(1, switch.reads.size)
        write.complete(Unit)
        runCurrent()
        assertEquals(2, switch.reads.size)
        assertEquals(true, access.state.value.on)
    }

    // ---- The entry ----

    @Test
    fun `the entry follows the link, the catalogue and the switch`() = test { access ->
        assertEquals(SshEntry.NotConnected, access.entry.value)
        inputs.connection.value = connected(CAR, ssh = null)
        runCurrent()
        assertEquals(SshEntry.NotInstalled(CAR, "Test car"), access.entry.value)
        inputs.catalogue.value = offering(sshVersion(">=2.0.0"))
        runCurrent()
        assertEquals(SshEntry.CantRun(CAR, "Test car"), access.entry.value)
        inputs.connection.value = connected(CAR)
        runCurrent()
        assertEquals(SshEntry.Ready(CAR, "Test car", on = false), access.entry.value)
        inputs.catalogue.value = CatalogueState()
        inputs.terminal.value = TerminalState.Open(CAR)
        runCurrent()
        assertEquals(SshEntry.Ready(CAR, "Test car", on = false, terminalOpen = true), access.entry.value)
    }

    // ---- What it says ----

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
    fun `the catalogue offers the package where a version of it runs on the agent`() {
        assertFalse(sshOffered(CatalogueState(), emptyList(), "1.7.0-beta.1", emptySet()))
        assertTrue(sshOffered(offering(sshVersion()), emptyList(), "1.7.0-beta.1", emptySet()))
        assertFalse(sshOffered(offering(sshVersion(">=2.0.0")), emptyList(), "1.7.0-beta.1", emptySet()))
        assertFalse(sshOffered(offering(), emptyList(), "1.7.0-beta.1", emptySet()))
    }
}

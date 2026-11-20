// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalState
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ConnectQuestionTest {
    private val names = mapOf("a" to "Car A", "b" to "Car B")
    private fun name(id: String) = names[id] ?: ConnectGate.OTHER

    private fun on(id: String) = SshPortState.On(id, port = 2222, hostKey = "SHA256:test")

    @Test
    fun `a terminal or the port of another head unit is asked about, each and both in one question`() {
        assertEquals(
            ConnectQuestion("Connect to Car A?", "A terminal is open on Car B. Connecting to Car A closes it."),
            connectQuestion("a", TerminalState.Open("b"), SshPortState.Off(), ::name),
        )
        // Being opened, and being switched on, count as open and on.
        assertEquals(
            "A terminal is open on Car B. Connecting to Car A closes it.",
            connectQuestion("a", TerminalState.Connecting("b"), SshPortState.Off(), ::name)?.text,
        )
        assertEquals(
            "SSH access for other apps is on for Car B. Connecting to Car A turns it off.",
            connectQuestion("a", TerminalState.Idle, SshPortState.Checking("b"), ::name)?.text,
        )
        assertEquals(
            "A terminal is open on Car B, and SSH access for other apps is on for it. Connecting to Car A closes the " +
                "terminal and turns SSH access for other apps off.",
            connectQuestion("a", TerminalState.Open("b", held = true), on("b"), ::name)?.text,
        )
        assertEquals(
            "SSH access for other apps is on for the other head unit. Connecting to Car A turns it off.",
            connectQuestion("a", TerminalState.Idle, on("gone"), ::name)?.text,
        )
    }

    @Test
    fun `nothing is asked for the same head unit, or where nothing is open`() {
        assertNull(connectQuestion("b", TerminalState.Open("b"), on("b"), ::name))
        assertNull(connectQuestion("a", TerminalState.Ended("b", TerminalEnd.ByUser), SshPortState.Off("b"), ::name))
        assertNull(connectQuestion("a", TerminalState.Idle, SshPortState.Off(), ::name))
    }

    private class Rig {
        val terminal = MutableStateFlow<TerminalState>(TerminalState.Open("b"))
        val port = MutableStateFlow<SshPortState>(SshPortState.On("b", port = 2222, hostKey = "SHA256:test"))
        val closes = AtomicInteger()
        val offs = AtomicInteger()
        val gate = ConnectGate(
            terminal,
            { closes.incrementAndGet(); terminal.value = TerminalState.Ended("b", TerminalEnd.ByUser) },
            port,
            { offs.incrementAndGet(); port.value = SshPortState.Off("b") },
            flowOf(mapOf("a" to "Car A", "b" to "Car B")),
        )
    }

    @Test
    fun `Cancel changes nothing, and Connect closes the terminal and switches the port off as the user would`() = runBlocking(Dispatchers.Default) {
        val rig = Rig()
        val cancelled = async { rig.gate.mayConnect("a") }
        val asked = withTimeout(5_000) { rig.gate.question.first { it != null } }
        assertEquals("Connect to Car A?", asked?.title)
        rig.gate.answer(false)
        assertFalse(cancelled.await())
        assertNull(rig.gate.question.value)
        assertEquals(0, rig.closes.get())
        assertEquals(0, rig.offs.get())
        assertEquals(TerminalState.Open("b"), rig.terminal.value)

        val connect = async { rig.gate.mayConnect("a") }
        withTimeout(5_000) { rig.gate.question.first { it != null } }
        rig.gate.answer(true)
        assertTrue(connect.await())
        assertEquals(1, rig.closes.get())
        assertEquals(1, rig.offs.get())
        assertEquals(TerminalState.Ended("b", TerminalEnd.ByUser), rig.terminal.value)
        assertNull(rig.gate.question.value)
    }

    @Test
    fun `the same head unit goes on at once, and a second question takes the first one's place`() = runBlocking(Dispatchers.Default) {
        val rig = Rig()
        assertTrue(rig.gate.mayConnect("b"))
        assertNull(rig.gate.question.value)

        val first = async { rig.gate.mayConnect("a") }
        withTimeout(5_000) { rig.gate.question.first { it != null } }
        val second = async { rig.gate.mayConnect("c") }
        withTimeout(5_000) { rig.gate.question.first { it?.title == "Connect to the other head unit?" } }
        assertFalse(first.await())
        rig.gate.answer(false)
        assertFalse(second.await())
        assertEquals(0, rig.closes.get())
    }
}

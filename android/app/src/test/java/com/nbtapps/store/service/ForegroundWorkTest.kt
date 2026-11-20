// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.service

import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.logs.LogPullState
import com.nbtapps.store.data.ssh.PortOffReason
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.data.terminal.TerminalEnd
import com.nbtapps.store.data.terminal.TerminalState
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.JobSnapshot
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.TcpEndpoint
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** What the foreground service runs for, and what its notification says: all of it but the drawing. */
class ForegroundWorkTest {

    private val units = listOf(HeadUnit("car", "Living room bench", listOf(TcpEndpoint("192.0.2.1", 7144))))

    private fun fetching(bytes: Long? = 250, total: Long? = 1_000) = JobView(
        "1011121314151617",
        "car",
        plan = null,
        job = JobSnapshot("1011121314151617", JobState.FETCHING, step = 1, steps = 2, phase = "fetch", bytes = bytes, total = total),
    )

    private val pulling = LogPullState.Pulling("org.nbtapp.agent", file = 2, files = 3, bytes = 500, total = 1_000)

    private fun notice(job: JobView? = null, pull: LogPullState? = null, terminal: TerminalState = TerminalState.Idle, port: SshPortState = SshPortState.Off()) =
        ForegroundWork.notice(ForegroundWork.reasons(job, pull, terminal, units, port))

    @Test
    fun `nothing to run for stops it, with no job that needs the phone, no pull, no terminal being opened or open`() {
        assertNull(notice())
        val ended = fetching().let { it.copy(job = it.job.copy(state = JobState.SUCCEEDED)) }
        assertNull(notice(job = ended))
        assertNull(notice(job = fetching().copy(linkLost = true)))
        assertNull(notice(pull = LogPullState.Done("org.nbtapp.agent", File("logs.zip"), emptyList())))
        for (state in listOf(
            TerminalState.Ended("car", TerminalEnd.ByUser),
            TerminalState.Failed("car", "No"),
            TerminalState.Off("car"),
            TerminalState.NoPackage("car"),
            TerminalState.Unsupported("car"),
        )) {
            assertNull("$state", notice(terminal = state))
        }
    }

    @Test
    fun `a terminal alone names the head unit as the app shows it, and holds no wake lock`() {
        assertEquals(
            ForegroundNotice("Terminal open on Living room bench", emptyList(), null, awake = false),
            notice(terminal = TerminalState.Open("car")),
        )
        assertEquals("Terminal open on Living room bench", notice(terminal = TerminalState.Open("car", held = true))?.title)
        assertEquals("Opening a terminal on Living room bench", notice(terminal = TerminalState.Connecting("car"))?.title)
        assertEquals("Terminal open on the head unit", notice(terminal = TerminalState.Open("gone"))?.title)
    }

    @Test
    fun `a tap goes to the terminal of the head unit it is open or being opened on, and otherwise only to the app`() {
        assertEquals("car", ForegroundWork.terminalToOpen(TerminalState.Open("car")))
        assertEquals("car", ForegroundWork.terminalToOpen(TerminalState.Open("car", held = true)))
        assertEquals("car", ForegroundWork.terminalToOpen(TerminalState.Connecting("car")))
        assertNull(ForegroundWork.terminalToOpen(TerminalState.Idle))
        assertNull(ForegroundWork.terminalToOpen(TerminalState.Ended("car", TerminalEnd.ByUser)))
        assertNull(ForegroundWork.terminalToOpen(TerminalState.Failed("car", "No")))
        assertNull(ForegroundWork.terminalToOpen(TerminalState.Off("car")))
    }

    @Test
    fun `the SSH port alone is the title, its port and connections the line, and its action is Turn off`() {
        fun on(connections: Int, paused: Boolean = false) =
            SshPortState.On("car", 2222, connections = connections, paused = paused, hostKey = "SHA256:x")
        assertEquals(
            ForegroundNotice("SSH access on for Living room bench", listOf("Port 2222 · no connections"), null, awake = false, portAction = "Turn off"),
            notice(port = on(0)),
        )
        assertEquals(listOf("Port 2222 · 1 connection"), notice(port = on(1))?.lines)
        assertEquals(listOf("Port 2222 · 2 connections"), notice(port = on(2))?.lines)
        assertEquals(listOf("Port 2222 · paused"), notice(port = on(1, paused = true))?.lines)
        assertEquals(
            ForegroundNotice("SSH access on for Living room bench", listOf("Checking the way to the head unit…"), null, awake = false, portAction = "Turn off"),
            notice(port = SshPortState.Checking("car")),
        )
        assertNull(notice(port = SshPortState.Off("car", reason = PortOffReason.IDLE)))
    }

    @Test
    fun `beside a terminal or a job the SSH port is a line, and its action is Turn off SSH port`() {
        val port = SshPortState.On("car", 2222, connections = 2, hostKey = "SHA256:x")
        assertEquals(
            ForegroundNotice("Terminal open on Living room bench", listOf("SSH access on · 2 connections"), null, awake = false, portAction = "Turn off SSH port"),
            notice(terminal = TerminalState.Open("car"), port = port),
        )
        val installing = notice(job = fetching(), port = port.copy(paused = true))
        // The install keeps its title, line and bar; the port is the line below.
        assertEquals(notice(job = fetching())?.title, installing?.title)
        assertEquals(notice(job = fetching())?.lines.orEmpty() + "SSH access on · paused", installing?.lines)
        assertEquals(notice(job = fetching())?.progress, installing?.progress)
        assertEquals("Turn off SSH port", installing?.portAction)
        // With no port there is no action.
        assertNull(notice(terminal = TerminalState.Open("car"))?.portAction)
    }

    @Test
    fun `a job or a pull stays the main text, and the terminal is a line below it`() {
        assertEquals(
            ForegroundNotice(
                "Transferring to the head unit",
                listOf("Step 1 of 2: Transferring · keep the ignition on", "Terminal open on Living room bench"),
                NoticeProgress(250),
                awake = true,
            ),
            notice(job = fetching(), terminal = TerminalState.Open("car")),
        )
        assertEquals(
            ForegroundNotice(
                "Getting logs from the head unit",
                listOf("File 2 of 3 · keep the ignition on", "Terminal open on Living room bench"),
                NoticeProgress(500),
                awake = true,
            ),
            notice(pull = pulling, terminal = TerminalState.Open("car")),
        )
        // All three: the job first, then a line each.
        assertEquals(
            listOf("Step 1 of 2: Transferring · keep the ignition on", "Getting logs from the head unit", "Terminal open on Living room bench"),
            notice(job = fetching(), pull = pulling, terminal = TerminalState.Open("car"))?.lines,
        )
    }

    @Test
    fun `a job alone is shown as it was, its bar moving with no end where it has no total, and kept awake only while it transfers`() {
        assertEquals(
            ForegroundNotice("Transferring to the head unit", listOf("Step 1 of 2: Transferring · keep the ignition on"), NoticeProgress(250), awake = true),
            notice(job = fetching()),
        )
        assertEquals(NoticeProgress(null), notice(job = fetching(bytes = null, total = null))?.progress)
        val installing = fetching().let { it.copy(job = it.job.copy(state = JobState.INSTALLING, phase = "files")) }
        assertEquals(false, notice(job = installing)?.awake)
        assertEquals(true, notice(job = installing.copy(reconnecting = true))?.awake)
        assertEquals(NoticeProgress(1_000), notice(pull = pulling.copy(bytes = 2_000))?.progress)
    }
}

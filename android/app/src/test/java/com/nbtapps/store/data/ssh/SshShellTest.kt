// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.data.jobs.FakeChannel
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.MemoryLink
import com.nbtapps.store.data.link.ProtocolConstants
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The shell's start where no SSH server is needed, against the test stand-in for the agent and its relay
 * ([FakeRelay]): no shell without a pty, the time limit on a server that never speaks, and a caller that gives up.
 * The shell against a real server is [SshRealAgentTest].
 */
class SshShellTest {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val sessions = CopyOnWriteArrayList<AgentSession>()

    @After
    fun tearDown() {
        sessions.forEach { it.close("the test is over") }
        scope.cancel()
    }

    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTimeout(30_000) { block() } }

    /** A tunnel to a relay that answers with [answer] and then takes what comes without a word. */
    private suspend fun tunnel(answer: String = ANSWER): SshTunnel {
        val (phone, other) = MemoryLink.pair()
        val fake = FakeAgent(other, "000102030405060708090a0b0c0d0e0f")
        fake.capabilities = """["${ProtocolConstants.CAPABILITY_CHANNELS}"]"""
        val relay = FakeRelay(answer).apply { echoes = false }
        fake.addChannel(FakeChannel(SshTunnel.PACKAGE, SshTunnel.CHANNEL, program = relay::program))
        val session = AgentSession(phone, scope).also { sessions += it }
        session.client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9")
        return SshTunnel.open(session, listOf(KEY.publicKeyLine()))
    }

    private fun noJschThread() = Thread.getAllStackTraces().keys.none { it.isAlive && it.name.startsWith("Connect thread") }

    @Test
    fun `a tunnel whose relay has no pty gets no shell, and is closed`() = test {
        val tunnel = tunnel("""{"v":1,"hostKeys":["$HOST_KEY"],"pty":false}""")
        val failed = runCatching { SshShell.open(tunnel, KEY, SshShell.ROOT, 80, 24, scope) }.exceptionOrNull()
        assertEquals(ShellEnd.NoPty, (failed as SshShellException).end)
        assertEquals(ChannelState.Closed(ChannelEnd.ByApp), tunnel.state.value)
    }

    @Test
    fun `a server that never answers is given up at the time limit, with nothing left`() = test {
        val tunnel = tunnel()
        val started = System.nanoTime()
        val failed = runCatching { SshShell.open(tunnel, KEY, SshShell.ROOT, 80, 24, scope, handshakeTimeoutMs = 500) }.exceptionOrNull()
        val took = (System.nanoTime() - started) / 1_000_000
        assertEquals(ShellEnd.Failed("The head unit's SSH server did not answer in time"), (failed as SshShellException).end)
        assertTrue("took $took ms", took in 500..5_000)
        assertTrue(tunnel.state.value is ChannelState.Closed)
        assertTrue(noJschThread())
    }

    @Test
    fun `a caller that gives up while the shell starts leaves nothing open`() = test {
        val tunnel = tunnel()
        val opening = async { SshShell.open(tunnel, KEY, SshShell.ROOT, 80, 24, scope) }
        delay(300)
        opening.cancelAndJoin()
        assertTrue(tunnel.state.value is ChannelState.Closed)
        assertTrue(noJschThread())
    }

    private companion object {
        val KEY = SshKey.generate()

        // Invented: its bytes count up, so only its layout is that of a real key.
        const val HOST_KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4f"
        const val ANSWER = """{"v":1,"hostKeys":["$HOST_KEY"],"pty":true}"""
    }
}

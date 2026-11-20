// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.channels.ChannelEnd
import com.nbtapps.store.data.channels.ChannelState
import com.nbtapps.store.data.jobs.FakeAgent
import com.nbtapps.store.data.jobs.FakeChannel
import com.nbtapps.store.data.jobs.Refusal
import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.MemoryLink
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.model.ChannelCloseReason
import com.nbtapps.store.model.ChannelError
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread
import kotlin.random.Random
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.JsonObject
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * The tunnel to the SSH relay ([SshTunnel]) against the test stand-in for the agent, whose channel plays the relay
 * ([FakeRelay]): the keys message and the answer, the two streams, how each side holds the other back, and every way
 * the open fails.
 */
class SshTunnelTest {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val sessions = CopyOnWriteArrayList<AgentSession>()

    @After
    fun tearDown() {
        sessions.forEach { it.close("the test is over") }
        scope.cancel()
    }

    // On the shared pool: the streams block, and what the link has started must run meanwhile.
    private fun test(block: suspend CoroutineScope.() -> Unit) = runBlocking(Dispatchers.Default) { withTimeout(30_000) { block() } }

    private class Rig(val fake: FakeAgent, val session: AgentSession, val relay: FakeRelay, val channel: FakeChannel)

    private suspend fun rig(answer: String? = ANSWER, switchedOn: Boolean = true): Rig {
        val (phone, other) = MemoryLink.pair()
        val fake = FakeAgent(other, "000102030405060708090a0b0c0d0e0f")
        fake.capabilities = """["${ProtocolConstants.CAPABILITY_CHANNELS}"]"""
        val relay = FakeRelay(answer)
        val channel = FakeChannel(SshTunnel.PACKAGE, SshTunnel.CHANNEL, switched = true, program = relay::program).apply { on = switchedOn }
        fake.addChannel(channel)
        val session = AgentSession(phone, scope).also { sessions += it }
        session.client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9")
        return Rig(fake, session, relay, channel)
    }

    private suspend fun failure(block: suspend () -> Unit): TunnelFailure = try {
        block()
        throw AssertionError("it opened")
    } catch (e: SshTunnelException) {
        e.failure
    }

    /** Waits for the phone to have told the fake agent `channel.close` with [reason]. */
    private fun FakeAgent.closed(reason: String = ChannelCloseReason.CLOSED) {
        val close = generateSequence { channelEvents.poll(5, TimeUnit.SECONDS) }.first { it["event"].toString() == "\"channel.close\"" }
        assertEquals("\"$reason\"", (close["params"] as JsonObject)["reason"].toString())
    }

    /** Runs [block], which blocks as a client's stream does, off the shared pool. */
    private suspend fun <T> io(block: () -> T): T = withContext(Dispatchers.IO) { block() }

    /** Reads [count] bytes from the tunnel, or as many as come before its end. */
    private fun SshTunnel.readBytes(count: Int): ByteArray {
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(1000)
        while (out.size() < count) {
            val n = input.read(buffer, 0, minOf(buffer.size, count - out.size()))
            if (n < 0) break
            out.write(buffer, 0, n)
        }
        return out.toByteArray()
    }

    @Test
    fun `the keys go in one message, the answer gives the host keys and pty, and then bytes go both ways in messages`() = test {
        val rig = rig()
        val tunnel = SshTunnel.open(rig.session, listOf(APP_KEY, OTHER_KEY))

        assertEquals(listOf("""{"v":1,"keys":["$APP_KEY","$OTHER_KEY"]}"""), rig.relay.keysMessages)
        assertEquals(listOf(HOST_KEY), tunnel.hostKeys)
        assertTrue(tunnel.pty)
        assertEquals("1.0.0", tunnel.version)
        assertEquals(ChannelState.Open, tunnel.state.value)

        // More than a message's worth at once, and a byte alone: cut into messages of 4,096 bytes at most.
        val sent = Random(7).nextBytes(10_000)
        var got: ByteArray? = null
        val reader = thread { got = tunnel.readBytes(sent.size + 1) }
        io {
            tunnel.output.write(sent)
            tunnel.output.write(42)
            reader.join(10_000)
        }
        assertFalse(reader.isAlive)
        assertArrayEquals(sent + byteArrayOf(42), got)
        val stream = rig.fake.openChannelStreams().single()
        assertEquals(listOf(4096, 4096, 1808, 1), rig.fake.channelMessages.getValue(stream).drop(1).map { it.size })

        tunnel.close()
        assertEquals(-1, io { tunnel.input.read() })
        assertEquals(ChannelState.Closed(ChannelEnd.ByApp), tunnel.state.value)
        rig.fake.closed()
        try {
            io { tunnel.output.write(1) }
            fail("written after the close")
        } catch (e: IOException) {
            // The channel has ended.
        }
    }

    @Test
    fun `an answer with pty false is told, and later members are not looked at`() = test {
        val rig = rig(answer = """{"v":1,"hostKeys":["$HOST_KEY","$OTHER_KEY"],"pty":false,"later":{"x":1}}""")
        val tunnel = SshTunnel.open(rig.session, listOf(APP_KEY))
        assertEquals(listOf(HOST_KEY, OTHER_KEY), tunnel.hostKeys)
        assertFalse(tunnel.pty)
        tunnel.close()
    }

    @Test
    fun `the agent's refusals of the open are told with their codes`() = test {
        // Switched off, as the switch is after every power down.
        val off = rig(switchedOn = false)
        assertEquals(ChannelError.OFF, (failure { SshTunnel.open(off.session, listOf(APP_KEY)) } as TunnelFailure.Refused).code)
        assertTrue(off.relay.keysMessages.isEmpty())

        // The package is not installed.
        val missing = rig()
        missing.fake.channels.clear()
        assertEquals(ChannelError.NOT_FOUND, (failure { SshTunnel.open(missing.session, listOf(APP_KEY)) } as TunnelFailure.Refused).code)

        // An agent without channels.
        val old = rig()
        old.fake.capabilities = "[]"
        assertEquals(ChannelError.UNSUPPORTED, (failure { SshTunnel.open(old.session, listOf(APP_KEY)) } as TunnelFailure.Refused).code)

        // As many channels open as a link may have.
        val full = rig()
        repeat(ProtocolConstants.CHANNEL_PER_LINK) { full.session.openChannel(SshTunnel.PACKAGE, SshTunnel.CHANNEL) }
        assertEquals(ChannelError.TOO_MANY, (failure { SshTunnel.open(full.session, listOf(APP_KEY)) } as TunnelFailure.Refused).code)

        // And those the stand-in has no reason of its own for: a job, a relay that did not start, the plugin's link.
        for (code in listOf(ChannelError.BUSY, ChannelError.UNAVAILABLE, ChannelError.FORBIDDEN, "later-code")) {
            val rig = rig()
            rig.fake.handlers["channel.open"] = { throw Refusal(code) }
            assertEquals(code, (failure { SshTunnel.open(rig.session, listOf(APP_KEY)) } as TunnelFailure.Refused).code)
        }
    }

    @Test
    fun `the relay's errors are told with their codes, and the channel is closed`() = test {
        for (code in listOf(RelayError.BAD_KEYS, RelayError.TOO_MANY, RelayError.FAILED, "no-pty")) {
            val rig = rig(answer = """{"error":"$code"}""")
            assertEquals(TunnelFailure.Relay(code), failure { SshTunnel.open(rig.session, listOf(APP_KEY)) })
            rig.fake.closed()
            assertTrue(rig.session.peer.openChannels().isEmpty())
        }
    }

    @Test
    fun `an answer that is not one the relay sends is told as such, and the channel is closed`() = test {
        val bad = listOf(
            "ssh-ed25519 AAAA",
            """{"v":2,"hostKeys":["$HOST_KEY"],"pty":true}""",
            """{"v":"1","hostKeys":["$HOST_KEY"],"pty":true}""",
            """{"v":1,"pty":true}""",
            """{"v":1,"hostKeys":[],"pty":true}""",
            """{"v":1,"hostKeys":[1],"pty":true}""",
            """{"v":1,"hostKeys":["no-pty $HOST_KEY"],"pty":true}""",
            """{"v":1,"hostKeys":["$HOST_KEY"]}""",
            """{"v":1,"hostKeys":["$HOST_KEY"],"pty":"yes"}""",
            """{"error":1}""",
            """{"v":1,"v":1,"hostKeys":["$HOST_KEY"],"pty":true}""",
            // The SSH server's first line, where the answer should be.
            "SSH-2.0-dropbear\r\n",
        )
        for (answer in bad) {
            val rig = rig(answer = answer)
            val told = failure { SshTunnel.open(rig.session, listOf(APP_KEY)) }
            assertTrue("$answer: $told", told is TunnelFailure.BadAnswer)
            rig.fake.closed()
        }
    }

    @Test
    fun `a channel that ends before the answer is told with the link layer's reason`() = test {
        // The relay closes its end, as it does with an agent line it cannot read.
        val closing = rig(answer = null)
        val opening = async { runCatching { SshTunnel.open(closing.session, listOf(APP_KEY)) }.exceptionOrNull() }
        while (closing.relay.keysMessages.isEmpty()) delay(10)
        closing.fake.closeChannel(closing.fake.openChannelStreams().single(), ChannelCloseReason.CLOSED)
        assertEquals(TunnelFailure.Closed(ChannelEnd.ByAgent(ChannelCloseReason.CLOSED)), (opening.await() as SshTunnelException).failure)

        // The switch turned off meanwhile.
        val off = rig(answer = null)
        val offOpening = async { runCatching { SshTunnel.open(off.session, listOf(APP_KEY)) }.exceptionOrNull() }
        while (off.relay.keysMessages.isEmpty()) delay(10)
        off.session.client.channelSet(SshTunnel.PACKAGE, SshTunnel.CHANNEL, false)
        assertEquals(TunnelFailure.Closed(ChannelEnd.ByAgent(ChannelCloseReason.OFF)), (offOpening.await() as SshTunnelException).failure)

        // The link lost.
        val lost = rig(answer = null)
        val lostOpening = async { runCatching { SshTunnel.open(lost.session, listOf(APP_KEY)) }.exceptionOrNull() }
        while (lost.relay.keysMessages.isEmpty()) delay(10)
        lost.fake.drop()
        assertTrue((lostOpening.await() as SshTunnelException).failure.let { it is TunnelFailure.Closed && it.end is ChannelEnd.LinkLost })
    }

    @Test
    fun `no answer in time, to the open or to the keys, is told, and leaves nothing open`() = test {
        val quiet = rig(answer = null)
        assertEquals(TunnelFailure.NoAnswer(toKeys = true), failure { SshTunnel.open(quiet.session, listOf(APP_KEY), answerTimeoutMs = 300) })
        quiet.fake.closed()
        assertTrue(quiet.session.peer.openChannels().isEmpty())

        val silent = rig()
        silent.fake.silent += "channel.open"
        assertEquals(TunnelFailure.NoAnswer(toKeys = false), failure { SshTunnel.open(silent.session, listOf(APP_KEY), openTimeoutMs = 300) })
        assertTrue(silent.session.peer.openChannels().isEmpty())
        assertTrue(silent.relay.keysMessages.isEmpty())
    }

    @Test
    fun `keys the relay would refuse are refused here, before anything is opened`() = test {
        val rig = rig()
        for (keys in listOf(emptyList(), List(9) { APP_KEY }, listOf(APP_KEY, "ssh-rsa AAAAB3NzaC1yc2E= me"))) {
            try {
                SshTunnel.open(rig.session, keys)
                fail("opened with $keys")
            } catch (e: IllegalArgumentException) {
                // As the relay would refuse it.
            }
        }
        assertTrue(rig.fake.requestsOf("channel.open").isEmpty())
    }

    @Test
    fun `the switch turned off ends the streams once what came is read, and a write then fails`() = test {
        val rig = rig()
        val tunnel = SshTunnel.open(rig.session, listOf(APP_KEY))
        val stream = rig.fake.openChannelStreams().single()
        rig.fake.programSends(stream, "last words".encodeToByteArray())
        rig.session.client.channelSet(SshTunnel.PACKAGE, SshTunnel.CHANNEL, false)
        tunnel.state.first { it is ChannelState.Closed }

        assertEquals("last words", io { tunnel.readBytes(100) }.decodeToString())
        assertEquals(-1, io { tunnel.input.read() })
        assertEquals(ChannelState.Closed(ChannelEnd.ByAgent(ChannelCloseReason.OFF)), tunnel.state.value)
        try {
            io { tunnel.output.write(1) }
            fail("written after the switch went off")
        } catch (e: IOException) {
            // The channel has ended.
        }
    }

    @Test
    fun `each side holds the other back, a relay that reads nothing stops the writer and a phone that reads nothing stops the relay`() = test {
        val rig = rig()
        val tunnel = SshTunnel.open(rig.session, listOf(APP_KEY))
        val stream = rig.fake.openChannelStreams().single()

        // The relay sends 100 messages: no more than the phone granted come while the phone reads nothing, and the
        // answer took one of those.
        repeat(100) { rig.fake.programSends(stream, "message $it;".encodeToByteArray()) }
        delay(300)
        assertEquals(100 - (ProtocolConstants.CHANNEL_CREDIT - 1), rig.fake.heldForPhone(stream))
        val expected = (0 until 100).joinToString("") { "message $it;" }
        assertEquals(expected, io { tunnel.readBytes(expected.length) }.decodeToString())

        // The relay stops reading: 16 messages go, and the 17th waits for credit until the tunnel is closed.
        rig.channel.takes = false
        io { repeat(ProtocolConstants.CHANNEL_CREDIT) { tunnel.output.write(byteArrayOf(it.toByte())) } }
        var failed: Throwable? = null
        val writer = thread { failed = runCatching { tunnel.output.write(byteArrayOf(99)) }.exceptionOrNull() }
        io { writer.join(300) }
        assertTrue("written with no credit", writer.isAlive)
        tunnel.close()
        io { writer.join(5_000) }
        assertFalse(writer.isAlive)
        assertTrue("was: $failed", failed is IOException)
    }

    private companion object {
        // Invented keys: their bytes count up, so only their layout is that of a real key.
        const val APP_KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g nbtapps"
        const val OTHER_KEY =
            "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8= laptop"
        const val HOST_KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4f"
        const val ANSWER = """{"v":1,"hostKeys":["$HOST_KEY"],"pty":true}"""
    }
}

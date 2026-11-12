// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.MemoryPairingKeys
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.json.StrictJson
import com.nbtapps.store.data.packages.PackageArchive
import com.nbtapps.store.data.packages.VerifiedPackage
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.InstallTarget
import com.nbtapps.store.model.JobState
import com.nbtapps.store.model.LinkClosing
import com.nbtapps.store.model.Offer
import com.nbtapps.store.model.PlanRequest
import com.nbtapps.store.model.RemoveTarget
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * Holds the conversation of `spec/protocol/session.vectors` from the phone's side. A stand-in for the agent
 * plays the agent's frames exactly as the real agent sent them, and checks every frame the app sends against
 * the one the file's phone sent: the same message, whatever the order of its members, and the same DATA bytes.
 */
class GoldenSessionTest {

    private class Step(val fromPhone: Boolean, val bytes: ByteArray, val what: String)

    private val lines = AgentVectors.file("protocol/session.vectors").readLines()

    private val steps: List<Step> = lines.mapIndexedNotNull { i, line ->
        when {
            line.startsWith("> ") -> Step(true, AgentVectors.hex(line.substring(2)), lines[i - 1])
            line.startsWith("< ") -> Step(false, AgentVectors.hex(line.substring(2)), lines[i - 1])
            else -> null
        }
    }

    private fun decode(bytes: ByteArray): Frame = FrameDecoder().run {
        feed(bytes, 0, bytes.size)
        requireNotNull(next())
    }

    /** Plays the agent's lines and checks the phone's. */
    private inner class ScriptedAgent(private val link: Link) : Thread("scripted-agent") {
        @Volatile
        var failure: String? = null

        @Volatile
        var finished = false

        override fun run() {
            try {
                val reader = FrameReader(link)
                if (!reader.raw(4).contentEquals(FrameCodec.MAGIC)) return fail("the link did not open with the magic bytes")
                for (step in steps) {
                    if (!step.fromPhone) {
                        link.output.write(step.bytes)
                        continue
                    }
                    val expected = decode(step.bytes)
                    val actual = reader.next() ?: return fail("the phone closed the link; expected ${step.what}")
                    if (actual.type != expected.type || actual.flags != expected.flags || actual.stream != expected.stream) {
                        return fail("expected ${step.what}, got a frame of type ${actual.type}, flags ${actual.flags}, stream ${actual.stream}")
                    }
                    val same = if (expected.type == FrameCodec.TYPE_MSG) {
                        StrictJson.parseObject(actual.payload) == StrictJson.parseObject(expected.payload)
                    } else {
                        actual.payload.contentEquals(expected.payload)
                    }
                    if (!same) return fail("expected ${step.what}\n but got ${actual.payload.decodeToString().take(600)}")
                }
                // The file ends with the agent's `link.closing`. This end stays open, as the agent's does, and
                // the next thing to come is the phone ending the link, with no frame before that.
                if (reader.next() != null) return fail("the phone sent a frame after link.closing")
                finished = true
            } catch (e: Exception) {
                fail("the stand-in agent stopped: $e")
            }
        }

        private fun fail(what: String) {
            failure = what
            link.close()
        }
    }

    private fun verified(name: String): Pair<File, VerifiedPackage> {
        val file = AgentVectors.file("packages/good/$name")
        return file to PackageArchive.verify(file, listOf(AgentVectors.packageKey))
    }

    private fun offer(p: VerifiedPackage) =
        Offer(p.manifest.id, p.manifest.version, p.sha256, p.size, p.manifestSha256, Hex.encode(p.signature))

    private fun blobs(file: File, p: VerifiedPackage): Map<String, Blob> =
        mapOf(p.sha256 to Blob.OfFile(file, p.size), p.manifestSha256 to Blob.OfBytes(p.manifestBytes))

    private suspend fun AgentSession.eventsUntilDone(): List<JobEvent> {
        val out = ArrayList<JobEvent>()
        for (event in events) {
            out += event
            if (event is JobEvent.Done) break
        }
        return out
    }

    private suspend fun codeOf(block: suspend () -> Unit): String = try {
        block()
        "no error"
    } catch (e: RemoteException) {
        e.code
    }

    @Test
    fun `the file is whole`() {
        assertTrue(steps.size >= 78)
        assertEquals("magic 4e42544c", lines.first { it.startsWith("magic") })
        // What the stand-in agent's last check rests on.
        assertFalse(steps.last().fromPhone)
        assertEquals("""# {"event":"link.closing","params":{"reason":"forgotten"}}""", steps.last().what)
    }

    @Test
    fun `the app says what the golden phone said and reads what the agent answered`() = runBlocking {
        // The file's link is a Bluetooth serial one: frames of 4,096 bytes, and a phone that is paired and proved
        // before it is told anything. Its clock stands still, so the keep-alive is put where it never comes due.
        val (phoneEnd, agentEnd) = MemoryLink.pair(kind = Link.SERIAL)
        val agent = ScriptedAgent(agentEnd).apply { start() }
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val session = AgentSession(phoneEnd, scope, pingAfterMs = 3_600_000, silenceMs = 7_200_000)
        val client = session.client
        val (helloFile, hello) = verified("org.nbtapp.hello-1.2.0.nbtpkg")
        val (runtimeFile, runtime) = verified("org.nbtapp.runtime-2.1.0.nbtpkg")
        val testKey = Hex.encode(AgentVectors.packageKey)
        try {
            withTimeout(30_000) {
                // Anything before hello is refused.
                assertEquals("bad-state", codeOf { session.peer.request("info.get") })

                val greeting = client.hello("5f2c9a1e07b34d6688a0c1d2e3f40516", "Pixel 9")
                val headUnitId = requireNotNull(greeting.headUnitId)
                assertEquals("000102030405060708090a0b0c0d0e0f", headUnitId)
                assertEquals("1.0.0", greeting.agentVersion)
                assertEquals(ProtocolConstants.SERIAL_MAX_FRAME, session.peer.maxFrame)

                // A phone that has not proved itself is told nothing, but its ping is answered.
                assertEquals("unauthenticated", codeOf { session.peer.request("info.get") })
                assertEquals("{}", session.peer.request("ping").toString())

                // Pairing: with no window open on the car, with a wrong code, and with the one the car shows.
                assertEquals("forbidden", codeOf { client.pairingIssue("000000") })
                assertEquals("rejected", codeOf { client.pairingIssue("951121") })
                val keys = MemoryPairingKeys()
                keys.put(headUnitId, client.pairingIssue("951120"))
                assertEquals("1415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f30313233", Hex.encode(keys.key(headUnitId)!!))

                // `auth` with the file's nonce. The agent's proof in the file is over the `hello` the file's phone
                // sent, byte for byte, so it only holds if the app's `hello` went as those bytes; and the stand-in
                // takes the app's proof only if it is the file's.
                client.auth(AgentVectors.hex("5a0f3c9d2e7b4a61c8d5e2f7a4b1c6d3")) { requireNotNull(keys.mac(headUnitId, it)) }

                val info = client.info()
                assertEquals("1.0.0", info.agentVersion)
                assertEquals(1, info.manifestVersion)
                assertEquals(500_000_000L, info.free.staging)
                assertEquals("none", info.restartNeeded)

                assertEquals(emptyList<Any>(), client.packages())
                val source = client.sources().single()
                assertEquals(testKey, source.key)
                assertTrue(source.embedded)

                assertEquals("unsupported", codeOf {
                    session.peer.request("source.add", buildJsonObject {
                        put("name", "Somebody else")
                        put("key", Hex.encode(AgentVectors.otherKey))
                    })
                })
                assertEquals("forbidden", codeOf { session.peer.request("source.remove", buildJsonObject { put("key", testKey) }) })
                assertEquals("{}", session.peer.request("ping").toString())

                val idle = client.status()
                assertNull(idle.job)
                assertTrue(idle.events.isEmpty())

                // A plan that cannot run: a dependency nobody offered, and a removal of something not installed.
                session.blobs.offer(blobs(helloFile, hello))
                val refused = client.plan(
                    PlanRequest(
                        install = listOf(InstallTarget("org.nbtapp.hello", "1.2.0")),
                        remove = listOf(RemoveTarget("org.nbtapp.old")),
                        offers = listOf(offer(hello)),
                    ),
                )
                assertNull(refused.planId)
                assertFalse(refused.runnable)
                assertEquals("org.nbtapp.runtime", refused.missing.single().id)
                assertEquals(">=2.0.0 <3.0.0", refused.missing.single().version)
                assertEquals("org.nbtapp.hello", refused.missing.single().requiredBy)
                assertEquals("not-installed", refused.problems.single().code)
                assertEquals(listOf("runs-scripts"), refused.flags)

                // A plan that can, and its job.
                session.blobs.offer(blobs(runtimeFile, runtime))
                val plan = client.plan(PlanRequest(install = listOf(InstallTarget("org.nbtapp.runtime", "2.1.0")), offers = listOf(offer(runtime))))
                assertEquals("4445464748494a4b", plan.planId)
                assertTrue(plan.runnable)
                assertEquals("install", plan.steps.single().action)
                assertEquals(956L, plan.fetchBytes)
                assertEquals(93L, plan.installBytes)

                session.blobs.jobStarted()
                val started = client.start(plan.planId!!)
                assertEquals(JobState.FETCHING, started.state)
                assertEquals(0L, started.seq)

                val events = session.eventsUntilDone()
                assertEquals((1L..11L).toList(), events.map { it.seq })
                val progress = events.filterIsInstance<JobEvent.Progress>()
                assertEquals(956L, progress[1].job.bytes)
                assertEquals(listOf("verify", "pre-install", "backup", "files", "post-install", "record"), progress.drop(3).map { it.job.phase })
                assertEquals("install of org.nbtapp.runtime 2.1.0 is complete\n", events.filterIsInstance<JobEvent.Log>().single().log.text)
                val done = events.last() as JobEvent.Done
                assertEquals(JobState.SUCCEEDED, done.result.state)
                assertEquals("done", done.result.steps.single().result)
                session.blobs.jobEnded()

                val status = client.status(plan.planId, after = 0)
                assertEquals(JobState.SUCCEEDED, status.job?.state)
                assertEquals(11L, status.job?.seq)
                assertNotNull(status.job?.result)
                assertEquals(listOf(10L, 11L), status.events.map { it.seq })
                assertTrue(status.events[0] is JobEvent.Log && status.events[1] is JobEvent.Done)
                assertFalse(status.more)

                val installed = client.packages().single()
                assertEquals("org.nbtapp.runtime", installed.id)
                assertEquals("2.1.0", installed.version)
                assertEquals(testKey, installed.key)

                assertEquals("too-late", codeOf { client.cancel(plan.planId) })

                // A removal, which goes straight to installing.
                val removal = client.plan(PlanRequest(remove = listOf(RemoveTarget("org.nbtapp.runtime", deleteData = true))))
                assertEquals("4c4d4e4f50515253", removal.planId)
                assertEquals("remove", removal.steps.single().action)
                assertEquals(JobState.INSTALLING, client.start(removal.planId!!).state)
                val removed = session.eventsUntilDone()
                assertEquals((1L..7L).toList(), removed.map { it.seq })
                assertEquals(JobState.SUCCEEDED, (removed.last() as JobEvent.Done).result.state)

                client.restart("hmi")

                // On the car this phone's pairing is ended. The agent says so before it ends the link, and the
                // app ends the link itself.
                assertEquals(AgentText.closing(LinkClosing.FORGOTTEN), session.ended.await())
                assertEquals(LinkClosing.FORGOTTEN, session.closing)
            }
        } catch (e: Exception) {
            agent.failure?.let { fail(it) }
            throw e
        } finally {
            session.close("the test is over")
            scope.cancel()
        }
        agent.join(5_000)
        assertNull(agent.failure)
        assertTrue("the conversation stopped before its end", agent.finished)
    }
}

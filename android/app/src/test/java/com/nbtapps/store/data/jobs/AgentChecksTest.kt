// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.data.link.AgentSession
import com.nbtapps.store.data.link.MemoryLink
import com.nbtapps.store.data.link.ProtocolException
import com.nbtapps.store.data.link.RemoteException
import com.nbtapps.store.model.CheckMove
import com.nbtapps.store.model.CheckState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.jsonPrimitive
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** `checks.run` and `checks.fix` as the client asks them, page by page, against a stand-in agent. */
class AgentChecksTest {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    @After
    fun tearDown() = scope.cancel()

    private val otherFiles = (1..5).map { "/opt/other/file$it" }
    private val demoFiles = (1..7).map { "/opt/demo/bin/demod$it" }
    private val demoMove = "/net/plugins/Demo" to "/opt/demo/Demo.old"

    private fun test(block: suspend (FakeAgent, AgentSession) -> Unit) = runBlocking(Dispatchers.Default) {
        val (phone, other) = MemoryLink.pair()
        val agent = FakeAgent(other, "000102030405060708090a0b0c0d0e0f")
        val session = AgentSession(phone, scope)
        try {
            withTimeout(30_000) { block(agent, session) }
        } finally {
            session.close("the test is over")
        }
    }

    @Test
    fun `every check is answered, and nothing is found by default`() = test { agent, session ->
        val checks = session.client.checksRun()
        assertEquals(listOf("system-writable", "aa-fix"), checks.map { it.id })
        assertTrue(checks.all { it.state == CheckState.OK && it.found.isEmpty() })
        assertEquals("{}", agent.requestsOf("checks.run").single().toString())
    }

    @Test
    fun `a list cut short, and the lists after it, are read page by page into whole lists`() = test { agent, session ->
        agent.checks.ids = listOf("other-check", "system-writable", "aa-fix")
        agent.checks.add(FakeCheck("other-check", otherFiles))
        agent.checks.add(FakeCheck("aa-fix", demoFiles, moves = listOf(demoMove)))
        agent.checks.pageSize = 3

        val checks = session.client.checksRun()

        assertEquals(otherFiles, checks[0].found)
        assertEquals(demoFiles, checks[2].found)
        assertFalse(checks.any { it.more })
        // The moves come whole with every page.
        assertEquals(listOf(CheckMove("/net/plugins/Demo", "/opt/demo/Demo.old")), checks[2].moves)
        assertEquals(emptyList<CheckMove>(), checks[0].moves)
        assertEquals(agent.checks.token("aa-fix", demoFiles, listOf(demoMove)), checks[2].fix?.token)
        // The first answer, one more page of the first check's, and three of the other's, from its start.
        val asked = agent.requestsOf("checks.run")
        assertEquals(5, asked.size)
        assertEquals("other-check", asked[1]["id"]?.jsonPrimitive?.content)
        assertEquals("/opt/other/file3", asked[1]["after"]?.jsonPrimitive?.content)
        assertEquals("aa-fix", asked[2]["id"]?.jsonPrimitive?.content)
        assertNull(asked[2]["after"])
    }

    @Test
    fun `a list that changes between two pages is read again from its start`() = test { agent, session ->
        val problem = FakeCheck("aa-fix", demoFiles)
        agent.checks.add(problem)
        agent.checks.pageSize = 4
        var pages = 0
        agent.checks.afterRun = { if (++pages == 1) problem.found = demoFiles + "/opt/demo/bin/demod8" }

        val check = session.client.checksRun("aa-fix").single()

        assertEquals(demoFiles + "/opt/demo/bin/demod8", check.found)
        assertEquals(agent.checks.token("aa-fix", check.found), check.fix?.token)
    }

    @Test
    fun `a list that keeps changing is given up on`() = test { agent, session ->
        val problem = FakeCheck("aa-fix", demoFiles)
        agent.checks.add(problem)
        agent.checks.pageSize = 2
        var pages = 0
        agent.checks.afterRun = { problem.found = demoFiles + "/opt/demo/new${++pages}" }

        try {
            session.client.checksRun()
            fail("a list that never holds still was taken as whole")
        } catch (e: ProtocolException) {
            assertTrue(e.message.orEmpty(), e.message.orEmpty().contains("kept changing"))
        }
    }

    @Test
    fun `a fix is sent with the token, and a stale one is refused`() = test { agent, session ->
        agent.checks.add(FakeCheck("aa-fix", demoFiles, moves = listOf(demoMove)))
        val token = session.client.checksRun("aa-fix").single().fix!!.token

        try {
            session.client.checksFix("aa-fix", "00".repeat(32))
            fail("a stale token was taken")
        } catch (e: RemoteException) {
            assertEquals("stale", e.code)
        }
        val result = session.client.checksFix("aa-fix", token)

        assertEquals(CheckState.OK, result.state)
        assertEquals(demoFiles.size, result.removed)
        assertEquals(listOf(CheckMove("/net/plugins/Demo", "/opt/demo/Demo.old")), result.moved)
        assertEquals(listOf("demod"), result.stopped)
        assertEquals(listOf("aa-fix" to token), agent.checks.fixes.drop(1))
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import java.io.File
import org.junit.Assert.assertArrayEquals
import org.junit.Test

/**
 * The copies under `src/test/resources/agent` against the agent repo they came from, `headunit/`, when it is
 * there. A difference means one side moved on: run `tools/sync_agent_vectors.sh`, then make the tests
 * pass again. Without the agent repo there is nothing to compare, and the test is skipped (fails under
 * `NBT_REQUIRE_BENCH`: see [AgentBench.need]).
 */
class AgentVectorsInSyncTest {

    /** `headunit/`, or where `NBT_AGENT_REPO` says: see [AgentBench]. */
    private val agentRepo = AgentBench.repo

    private fun assertSame(copy: String, original: String) {
        val source = File(agentRepo, original)
        AgentBench.need("no agent repo at ${agentRepo}", source.isFile)
        assertArrayEquals("$copy differs from the agent repo's $original", source.readBytes(), AgentVectors.file(copy).readBytes())
    }

    @Test
    fun `the JSON profile vectors are the agent repo's`() = assertSame("json-profile.vectors", "spec/json-profile.vectors")

    @Test
    fun `the frame vectors are the agent repo's`() = assertSame("protocol/frames.vectors", "spec/protocol/frames.vectors")

    @Test
    fun `the session vectors are the agent repo's`() = assertSame("protocol/session.vectors", "spec/protocol/session.vectors")

    @Test
    fun `the protocol constants are the agent repo's`() = assertSame("protocol/constants.txt", "spec/protocol/constants.txt")

    @Test
    fun `the test keys are the agent repo's`() {
        for (name in listOf("test-package", "test-other", "test-index")) assertSame("keys/$name.pub", "tests/keys/$name.pub")
    }

    @Test
    fun `the test packages are the ones the agent repo builds`() {
        // The agent repo builds them into its build folder; they are compared only once it has.
        assertSame("packages/vectors.json", "build/testdata/vectors.json")
        for (kind in listOf("good", "bad", "detached")) {
            for (file in AgentVectors.file("packages/$kind").listFiles().orEmpty()) {
                assertSame("packages/$kind/${file.name}", "build/testdata/$kind/${file.name}")
            }
        }
    }
}

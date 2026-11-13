// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import java.io.File
import java.nio.file.Files
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The copies under `src/test/resources/agent` against the agent repo they came from, `headunit/`, when it is
 * there. A difference means one side moved on: run `tools/sync_agent_vectors.sh`, then make the tests
 * pass again. Without the agent repo there is nothing to compare, and the test is skipped (fails under
 * `NBT_REQUIRE_BENCH`: see [AgentBench.need]); the test packages also need its bench, and so Docker.
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
        // Built afresh in the agent repo's bench, as tools/sync_agent_vectors.sh builds the copies: compared byte
        // for byte, they have to come from the same zlib, and only the bench's pinned Debian gives every machine
        // the same one. Its own `make testdata` uses this machine's.
        val script = File(agentRepo, "tools/bench.py")
        AgentBench.need("no agent repo at ${agentRepo}", script.isFile)
        val built = Files.createTempDirectory("nbt-testdata").toFile()
        try {
            val said = runCatching {
                val bench = ProcessBuilder("python3", script.path, "testdata", built.path).redirectErrorStream(true).start()
                val output = bench.inputStream.readBytes().decodeToString()
                if (bench.waitFor(5, TimeUnit.MINUTES) && bench.exitValue() == 0) null else output
            }.getOrElse { it.toString() }
            AgentBench.need("the agent repo's bench could not build the test packages: $said", said == null)

            assertArrayEquals("packages/vectors.json differs from the bench's", File(built, "vectors.json").readBytes(), AgentVectors.file("packages/vectors.json").readBytes())
            for (kind in listOf("good", "bad", "detached")) {
                val copies = AgentVectors.file("packages/$kind").listFiles().orEmpty().map { it.name }.sorted()
                assertEquals("the $kind packages", File(built, kind).listFiles().orEmpty().map { it.name }.sorted(), copies)
                for (name in copies) {
                    assertArrayEquals("packages/$kind/$name differs from the bench's", File(built, "$kind/$name").readBytes(), AgentVectors.file("packages/$kind/$name").readBytes())
                }
            }
        } finally {
            built.deleteRecursively()
        }
    }
}

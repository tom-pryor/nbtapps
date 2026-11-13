// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import java.io.File
import java.util.concurrent.TimeUnit
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue

/**
 * The real agent for a test: the agent repo's `nbtagent`, run by its `tools/bench.py` in a Docker container.
 *
 * The agent runs the hook scripts of the packages a test installs, and a script is an ordinary process of
 * whoever started the agent. In the container it sees the head unit's folder, the socket's folder and the
 * test key, and nothing else of this machine.
 *
 * It needs the agent repo, Docker, and the bench image (`make bench-image` in the agent repo). A test asks
 * [available] first, through [need], and is skipped without them.
 *
 * The agent repo is this repo's `headunit/` folder, next to `android/`. Where it is somewhere else, the environment
 * variable `NBT_AGENT_REPO` names its folder; `NBT_BENCH_IMAGE`,
 * which the agent repo's `tools/bench.py` reads, names an image other than `nbtapp-bench`.
 */
object AgentBench {
    // Unit tests run with the module folder as the working directory.
    val repo: File = (System.getenv("NBT_AGENT_REPO")?.takeIf { it.isNotEmpty() }?.let(::File) ?: File("../../headunit")).absoluteFile.normalize()
    private val script = File(repo, "tools/bench.py")

    /**
     * The version the agent repo's agent says it is, from its `agent/version.hpp`: what a test expects of the
     * agent it started, whichever version that repo is at.
     */
    val version: String by lazy {
        val header = File(repo, "agent/version.hpp")
        Regex("""kAgentVersion\[] = "([^"]+)"""").find(header.readText())?.groupValues?.get(1)
            ?: error("$header names no version")
    }

    const val MISSING ="no agent bench: it needs the agent repo (../headunit, or what NBT_AGENT_REPO names), Docker and `make bench-image` there"

    /**
     * Set when the environment variable `NBT_REQUIRE_BENCH` is: CI sets it, so that a bench or an agent repo that
     * is missing there fails the run, and does not leave it passing with these tests skipped.
     */
    private val required = !System.getenv("NBT_REQUIRE_BENCH").isNullOrEmpty()

    /**
     * What a test that needs the bench, the agent repo or one of its tools asks before it starts: without [present]
     * the test is skipped, as `assumeTrue` skips it, or fails when [required] is set. [reason] says what is missing.
     */
    fun need(reason: String, present: Boolean) {
        if (present) return
        if (required) fail("NBT_REQUIRE_BENCH is set, and $reason")
        assumeTrue(reason, false)
    }

    /** True if the container can be started. Asked once for a test run. */
    val available: Boolean by lazy {
        script.isFile && runCatching {
            val asked = ProcessBuilder("python3", script.path, "have").redirectErrorStream(true).start()
            asked.inputStream.readBytes() // it prints nothing worth keeping; read so that it cannot block
            asked.waitFor(30, TimeUnit.SECONDS) && asked.exitValue() == 0
        }.getOrDefault(false)
    }

    /**
     * Starts the agent on [root], which the container may write, listening at [socket]. The socket's folder is
     * handed to the container too, so it has to be one that holds nothing else. With [debugTcp] the agent also
     * listens on a free port of this machine's loopback address, and says which in [log].
     *
     * The process is the container's: `destroy()` stops the agent as SIGTERM does.
     */
    fun start(root: File, socket: File, log: File, debugTcp: Boolean = false): Process {
        val command = mutableListOf(
            "python3", script.path, "agent", "--root", root.path, "--socket", socket.path,
            "--test-key", AgentVectors.file("keys/test-package.pub").absolutePath,
        )
        if (debugTcp) command += listOf("--debug-tcp", "0")
        return ProcessBuilder(command).redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.appendTo(log)).start()
    }

    /** Stops an agent that [start] started, and waits for its container to be gone. */
    fun stop(agent: Process) {
        agent.destroy()
        if (!agent.waitFor(20, TimeUnit.SECONDS)) agent.destroyForcibly()
    }
}

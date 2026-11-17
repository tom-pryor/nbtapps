// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import com.nbtapps.store.data.crypto.Hex
import java.io.ByteArrayOutputStream
import java.io.File

/** The files copied from the agent repo by `tools/sync_agent_vectors.sh`: what both sides are tested against. */
object AgentVectors {
    // Unit tests run with the module folder as the working directory.
    val root = File("src/test/resources/agent")

    fun file(path: String): File = File(root, path)

    /** The key in a key file of the agent repo: one line of 64 hex characters, with # lines as comments. */
    fun key(name: String): ByteArray {
        val line = file("keys/$name.pub").readLines().first { it.isNotBlank() && !it.startsWith("#") }
        return requireNotNull(Hex.decodeOrNull(line.trim(), 32)) { "keys/$name.pub holds no key" }
    }

    val packageKey: ByteArray get() = key("test-package")
    val otherKey: ByteArray get() = key("test-other")
    val indexKey: ByteArray get() = key("test-index")

    fun hex(text: String): ByteArray = requireNotNull(Hex.decodeOrNull(text, text.length / 2)) { "not hex: $text" }

    /** One case of a `.vectors` file: its line number, its verdict and its input. */
    class Case(val line: Int, val verdict: String, val input: ByteArray)

    /**
     * The cases of a `.vectors` file such as `log-paths.vectors`: a verdict, a space and the input, where `%HH` stands
     * for the byte HH. `#` lines are comments, and a note may follow two spaces and a #.
     */
    fun cases(path: String): List<Case> = file(path).readLines().mapIndexedNotNull { index, line ->
        if (line.isBlank() || line.startsWith("#")) return@mapIndexedNotNull null
        val text = line.substringBefore("  #")
        val out = ByteArrayOutputStream()
        val input = text.substringAfter(' ', "")
        var i = 0
        while (i < input.length) {
            if (input[i] == '%') {
                out.write(input.substring(i + 1, i + 3).toInt(16))
                i += 3
            } else {
                out.write(input[i].toString().encodeToByteArray())
                i++
            }
        }
        Case(index + 1, text.substringBefore(' '), out.toByteArray())
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import com.nbtapps.store.data.crypto.Hex
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
}

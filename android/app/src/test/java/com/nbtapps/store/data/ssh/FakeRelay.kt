// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList

/**
 * The SSH relay as the program of a test agent's channel ([com.nbtapps.store.data.jobs.FakeChannel.program]): the
 * first message of each channel is the keys message, which gets [answer] (none when it is null); then each message
 * comes back as it went while [echoes], and is taken without a word when not, as by a server that never speaks.
 */
class FakeRelay(@Volatile var answer: String?) {
    /** The keys messages, as they came. */
    val keysMessages = CopyOnWriteArrayList<String>()

    @Volatile
    var echoes = true

    private val answered: MutableSet<Int> = ConcurrentHashMap.newKeySet()

    fun program(stream: Int, message: ByteArray): List<ByteArray> {
        if (!answered.add(stream)) return if (echoes) listOf(message) else emptyList()
        keysMessages += message.decodeToString()
        return listOfNotNull(answer?.encodeToByteArray())
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** HMAC-SHA256 of [message] under [key]: what a pairing key is for, worked out the plain way. */
fun hmacSha256(key: ByteArray, message: ByteArray): ByteArray =
    Mac.getInstance(PairingKeys.ALGORITHM).run {
        init(SecretKeySpec(key, PairingKeys.ALGORITHM))
        doFinal(message)
    }

/** Keeps pairing keys in memory, for tests: the Android Keystore is not there on the JVM. */
class MemoryPairingKeys : PairingKeys {
    private val keys = ConcurrentHashMap<String, ByteArray>()

    /** When set, every call fails with it, as a key store that is out of order does. */
    @Volatile
    var broken: String? = null

    /** The head units a key is kept for. */
    val headUnitIds: Set<String> get() = keys.keys.toSet()

    /** The key kept for [headUnitId], which the real store never gives. */
    fun key(headUnitId: String): ByteArray? = keys[headUnitId]?.copyOf()

    override suspend fun has(headUnitId: String): Boolean = working { keys.containsKey(headUnitId) }

    override suspend fun put(headUnitId: String, key: ByteArray) = working {
        require(key.size == PairingKeys.KEY_BYTES)
        // A copy: the caller wipes its own once the key is kept.
        keys[headUnitId] = key.copyOf()
    }

    override suspend fun remove(headUnitId: String) = working<Unit> { keys.remove(headUnitId) }

    override suspend fun mac(headUnitId: String, message: ByteArray): ByteArray? = working { keys[headUnitId]?.let { hmacSha256(it, message) } }

    private fun <T> working(block: () -> T): T {
        broken?.let { throw IOException(it) }
        return block()
    }
}

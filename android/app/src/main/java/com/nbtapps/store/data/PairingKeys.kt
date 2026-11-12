// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import android.security.keystore.KeyProperties
import android.security.keystore.KeyProtection
import java.io.IOException
import java.security.GeneralSecurityException
import java.security.KeyStore
import java.security.ProviderException
import javax.crypto.Mac
import javax.crypto.SecretKey
import javax.crypto.spec.SecretKeySpec
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * The pairing keys head units have issued to this phone: one to a head unit, filed under the id its agent gives
 * in `hello`. A key is 32 bytes that the agent made when the user typed the code from the car's screen into the
 * app (`pairing.issue`), and it is what `auth` proves on every link after that.
 *
 * A key goes in and never comes out: all that can be had of it is an HMAC-SHA256 under it, which is all `auth`
 * needs.
 *
 * @throws IOException from any of these, if the place the keys are kept fails.
 */
interface PairingKeys {
    suspend fun has(headUnitId: String): Boolean

    /** Keeps [key] for [headUnitId] in place of any it had. */
    suspend fun put(headUnitId: String, key: ByteArray)

    /** Does nothing if there is none. */
    suspend fun remove(headUnitId: String)

    /** HMAC-SHA256 of [message] under the key of [headUnitId], or null if it has none. */
    suspend fun mac(headUnitId: String, message: ByteArray): ByteArray?

    companion object {
        const val KEY_BYTES = 32
        const val ALGORITHM = "HmacSHA256"
    }
}

/**
 * The keys in the Android Keystore, as HMAC keys that cannot be exported: the app, and anything that reads the
 * app's files, can use one while the phone runs and cannot copy it. They go when the app is uninstalled or its
 * data cleared, and the phone then pairs again.
 */
class KeystorePairingKeys(private val io: CoroutineDispatcher = Dispatchers.IO) : PairingKeys {
    private val lock = Mutex()
    private var opened: KeyStore? = null

    override suspend fun has(headUnitId: String): Boolean = withStore { it.containsAlias(alias(headUnitId)) }

    override suspend fun put(headUnitId: String, key: ByteArray) {
        require(key.size == PairingKeys.KEY_BYTES) { "a pairing key is ${PairingKeys.KEY_BYTES} bytes" }
        withStore {
            it.setEntry(
                alias(headUnitId),
                KeyStore.SecretKeyEntry(SecretKeySpec(key, PairingKeys.ALGORITHM)),
                // An HMAC is a signature to the Keystore, and the key's algorithm names the one hash it works with.
                KeyProtection.Builder(KeyProperties.PURPOSE_SIGN).build(),
            )
        }
    }

    override suspend fun remove(headUnitId: String) = withStore {
        if (it.containsAlias(alias(headUnitId))) it.deleteEntry(alias(headUnitId))
    }

    override suspend fun mac(headUnitId: String, message: ByteArray): ByteArray? = withStore {
        val key = it.getKey(alias(headUnitId), null) as? SecretKey ?: return@withStore null
        Mac.getInstance(PairingKeys.ALGORITHM).run {
            init(key)
            doFinal(message)
        }
    }

    private fun alias(headUnitId: String) = "pairing-$headUnitId"

    /** The Keystore answers over a binder call, so off the caller's thread, and one call at a time. */
    private suspend fun <T> withStore(block: (KeyStore) -> T): T = lock.withLock {
        withContext(io) {
            try {
                val store = opened ?: KeyStore.getInstance("AndroidKeyStore").apply { load(null) }.also { opened = it }
                block(store)
            } catch (e: GeneralSecurityException) {
                throw IOException("The phone's key store failed: ${e.message ?: e.javaClass.simpleName}", e)
            } catch (e: ProviderException) {
                throw IOException("The phone's key store failed: ${e.message ?: e.javaClass.simpleName}", e)
            }
        }
    }
}

/**
 * Removes the saved head unit [id], and with it the key its agent issued this phone, unless another saved entry
 * is the same head unit. A key that cannot be removed stays: it is of no use without the head unit, and a new
 * pairing writes over it.
 */
suspend fun HeadUnitRepository.removeWithKey(id: String, keys: PairingKeys) {
    val agentId = headUnit(id).first()?.headUnitId
    remove(id)
    if (agentId == null || headUnits.first().any { it.headUnitId == agentId }) return
    try {
        keys.remove(agentId)
    } catch (e: IOException) {
        // See above.
    }
}

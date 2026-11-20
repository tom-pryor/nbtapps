// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.File
import java.io.IOException
import java.security.GeneralSecurityException
import java.security.KeyStore
import java.security.ProviderException
import java.security.SecureRandom
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.bouncycastle.crypto.generators.Ed25519KeyPairGenerator
import org.bouncycastle.crypto.params.Ed25519KeyGenerationParameters
import org.bouncycastle.crypto.params.Ed25519PrivateKeyParameters
import org.bouncycastle.crypto.signers.Ed25519Signer

/**
 * An ed25519 key for SSH, held and used with Bouncy Castle's own classes, as [com.nbtapps.store.data.crypto.Ed25519]
 * verifies: no security provider is asked, so it signs the same on every Android version. What it gives out is its
 * public half and signatures; the private half is never encoded for anything else.
 */
class SshKey private constructor(private val private: Ed25519PrivateKeyParameters) {
    /** The public key's 32 bytes. */
    val publicKey: ByteArray = private.generatePublicKey().encoded

    /** The public key as SSH encodes it: the string `ssh-ed25519` and the string of its 32 bytes. */
    val blob: ByteArray = SshKeyLines.sshStrings(SshKeyLines.ED25519.encodeToByteArray(), publicKey)

    /** The public key as one line of `authorized_keys`, with [comment]: `ssh-ed25519 AAAA... comment`. */
    fun publicKeyLine(comment: String = COMMENT): String = "${SshKeyLines.ED25519} ${Base64.getEncoder().encodeToString(blob)} $comment"

    /** The key's fingerprint as OpenSSH shows it. */
    val fingerprint: String get() = SshKeyLines.fingerprint(blob)

    /** The Ed25519 signature of [data] (RFC 8032), 64 bytes. */
    fun sign(data: ByteArray): ByteArray = Ed25519Signer().run {
        init(true, private)
        update(data, 0, data.size)
        generateSignature()
    }

    /** The signature of [data] as SSH sends it in a login: the string `ssh-ed25519` and the string of the signature. */
    fun sshSignature(data: ByteArray): ByteArray = SshKeyLines.sshStrings(SshKeyLines.ED25519.encodeToByteArray(), sign(data))

    /** The 32 bytes the key is made from, for [SshIdentity] to keep. */
    internal fun seed(): ByteArray = private.encoded

    companion object {
        /** What the app's own key line says of it. */
        const val COMMENT = "nbtapps"

        const val SEED_BYTES = 32

        /** A new key. */
        fun generate(random: SecureRandom = SecureRandom()): SshKey = Ed25519KeyPairGenerator().run {
            init(Ed25519KeyGenerationParameters(random))
            SshKey(generateKeyPair().private as Ed25519PrivateKeyParameters)
        }

        /** The key made from [seed], 32 bytes. */
        fun fromSeed(seed: ByteArray): SshKey {
            require(seed.size == SEED_BYTES) { "an ed25519 key is made from $SEED_BYTES bytes" }
            return SshKey(Ed25519PrivateKeyParameters(seed, 0))
        }
    }
}

/**
 * The app's own SSH key: what it logs in to a head unit's SSH server with. It is made the first time it is asked for
 * and kept from then on. Its public line goes to the head unit in the keys message of every connection, which is all
 * the head unit knows of it; so a key that is lost and made again costs nothing.
 *
 * @throws IOException from [key] if the place it is kept fails.
 */
interface SshIdentity {
    suspend fun key(): SshKey

    /** The public key as one line of `authorized_keys`: what the keys message carries for the app. */
    suspend fun publicKeyLine(): String = key().publicKeyLine()
}

/**
 * The key kept where the pairing keys are, in the Android Keystore ([com.nbtapps.store.data.KeystorePairingKeys]),
 * as far as an ed25519 key made with Bouncy Castle can be: the Keystore cannot hold such a key and sign with Bouncy
 * Castle, so it holds an AES key that cannot be exported, and [file], in the app's own files, holds the key's 32 bytes
 * encrypted under it (AES-GCM). Without the phone's Keystore the file is of no use: a copy of the app's files does
 * not give the key. Both go when the app is uninstalled or its data cleared, as the pairing keys do; and since
 * neither the cloud backup nor a device transfer takes the app's files, a new key is made on a new phone.
 *
 * A file that cannot be read, or that the Keystore's key no longer opens, is written over with a new key.
 */
class KeystoreSshIdentity(
    private val file: File,
    private val io: CoroutineDispatcher = Dispatchers.IO,
) : SshIdentity {
    private val lock = Mutex()
    private var cached: SshKey? = null

    override suspend fun key(): SshKey = lock.withLock {
        cached ?: withContext(io) {
            try {
                val store = KeyStore.getInstance(KEYSTORE).apply { load(null) }
                read(store) ?: make(store)
            } catch (e: GeneralSecurityException) {
                throw IOException("The phone's key store failed: ${e.message ?: e.javaClass.simpleName}", e)
            } catch (e: ProviderException) {
                throw IOException("The phone's key store failed: ${e.message ?: e.javaClass.simpleName}", e)
            }
        }.also { cached = it }
    }

    /** The key the file holds, or null where there is none to be had. */
    private fun read(store: KeyStore): SshKey? {
        val bytes = try {
            file.readBytes()
        } catch (e: IOException) {
            return null
        }
        if (bytes.size != 1 + IV_BYTES + SshKey.SEED_BYTES + TAG_BYTES || bytes[0] != FORMAT) return null
        val seed = try {
            val wrapping = store.getKey(ALIAS, null) as? SecretKey ?: return null
            Cipher.getInstance(CIPHER).run {
                init(Cipher.DECRYPT_MODE, wrapping, GCMParameterSpec(TAG_BYTES * 8, bytes, 1, IV_BYTES))
                doFinal(bytes, 1 + IV_BYTES, bytes.size - 1 - IV_BYTES)
            }
        } catch (e: GeneralSecurityException) {
            // Another key, or one the Keystore no longer opens (KeyPermanentlyInvalidatedException among them).
            return null
        } catch (e: ProviderException) {
            return null
        }
        return try {
            SshKey.fromSeed(seed)
        } finally {
            seed.fill(0)
        }
    }

    /** A new key, kept in the file under a new Keystore key, written whole or not at all. */
    private fun make(store: KeyStore): SshKey {
        if (store.containsAlias(ALIAS)) store.deleteEntry(ALIAS)
        val wrapping = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE).run {
            init(
                KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .build(),
            )
            generateKey()
        }
        val key = SshKey.generate()
        val seed = key.seed()
        try {
            // The Keystore picks the IV.
            val cipher = Cipher.getInstance(CIPHER).apply { init(Cipher.ENCRYPT_MODE, wrapping) }
            val sealed = cipher.doFinal(seed)
            val iv = cipher.iv
            check(iv.size == IV_BYTES && sealed.size == SshKey.SEED_BYTES + TAG_BYTES) { "the key store's AES-GCM is not as asked" }
            file.parentFile?.mkdirs()
            val temporary = File(file.path + ".new")
            temporary.writeBytes(byteArrayOf(FORMAT) + iv + sealed)
            if (!temporary.renameTo(file)) throw IOException("The SSH key could not be saved")
        } finally {
            seed.fill(0)
        }
        return key
    }

    private companion object {
        const val KEYSTORE = "AndroidKeyStore"
        const val ALIAS = "ssh-key"
        const val CIPHER = "AES/GCM/NoPadding"
        const val FORMAT: Byte = 1
        const val IV_BYTES = 12
        const val TAG_BYTES = 16
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.crypto

import org.bouncycastle.crypto.params.Ed25519PublicKeyParameters
import org.bouncycastle.crypto.signers.Ed25519Signer

/**
 * Ed25519 signature verification (RFC 8032). The app only ever verifies: a catalogue index against its
 * source's index key, and a package's `manifest.sig` against the source's package key.
 *
 * Bouncy Castle does the work, through its own classes and not as a security provider, so the answer is the
 * same on every Android version and in the unit tests, whatever Ed25519 the platform has or lacks.
 *
 * The head-unit agent verifies with TweetNaCl plus a check that the second half of the signature is below
 * the group order. The two agree on every signature an honest signer makes, and both refuse a second half at
 * or above the group order. They can differ on keys and signatures built to sit on the edges: Bouncy Castle
 * refuses a public key of small order, for one, and TweetNaCl takes it. The agent's answer is the one that
 * decides what is installed; for a package the app's check only saves a transfer the agent would refuse.
 */
object Ed25519 {
    const val KEY_BYTES = 32
    const val SIGNATURE_BYTES = 64

    /** True if [signature] is the signature of [message] under [publicKey]. */
    fun verify(publicKey: ByteArray, message: ByteArray, signature: ByteArray): Boolean {
        if (publicKey.size != KEY_BYTES || signature.size != SIGNATURE_BYTES) return false
        val key = try {
            Ed25519PublicKeyParameters(publicKey, 0)
        } catch (e: IllegalArgumentException) {
            return false // not the encoding of a point this library accepts as a key
        }
        val signer = Ed25519Signer()
        signer.init(false, key)
        signer.update(message, 0, message.size)
        return signer.verifySignature(signature)
    }

    /**
     * The first 16 hex characters of the SHA-256 of a public key, in four groups: what the app and the car's
     * prompt both show for a source's key.
     */
    fun fingerprint(publicKey: ByteArray): String = sha256Hex(publicKey).take(16).chunked(4).joinToString(" ")
}

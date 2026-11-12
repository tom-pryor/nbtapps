// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.Signature

/** An Ed25519 key pair made by the JDK, for tests that need to sign. The app itself never signs. */
class TestKeys {
    private val pair: KeyPair = KeyPairGenerator.getInstance("Ed25519").generateKeyPair()

    /** The 32 bytes of the public key: the end of its X.509 encoding. */
    val publicKey: ByteArray = pair.public.encoded.let { it.copyOfRange(it.size - 32, it.size) }

    fun sign(message: ByteArray): ByteArray =
        Signature.getInstance("Ed25519").run {
            initSign(pair.private)
            update(message)
            sign()
        }
}

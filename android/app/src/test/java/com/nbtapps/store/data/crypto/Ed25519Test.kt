// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.crypto

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.TestKeys
import java.math.BigInteger
import kotlin.random.Random
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class Ed25519Test {

    @Test
    fun `verifies the first test vector of RFC 8032`() {
        val key = AgentVectors.hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        val signature = AgentVectors.hex(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155" +
                "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        )
        assertTrue(Ed25519.verify(key, ByteArray(0), signature))
        assertFalse(Ed25519.verify(key, byteArrayOf(0), signature))
    }

    @Test
    fun `agrees with the JDK on signatures it made, and refuses them once anything changes`() {
        val random = Random(8032)
        repeat(12) { round ->
            val keys = TestKeys()
            val message = random.nextBytes(round * 37)
            val signature = keys.sign(message)

            assertTrue(Ed25519.verify(keys.publicKey, message, signature))
            assertFalse(Ed25519.verify(keys.publicKey, message + 1, signature))
            for (at in listOf(0, 31, 32, 63)) {
                val changed = signature.copyOf().also { it[at] = (it[at].toInt() xor 0x04).toByte() }
                assertFalse("signature byte $at changed", Ed25519.verify(keys.publicKey, message, changed))
            }
            assertFalse(Ed25519.verify(TestKeys().publicKey, message, signature))
        }
    }

    @Test
    fun `a signature with the group order added to its second half is refused`() {
        // The same point on the curve, so a verifier that reduces instead of checking would take it.
        val order = BigInteger.valueOf(2).pow(252) + BigInteger("27742317777372353535851937790883648493")
        val keys = TestKeys()
        val message = "manifest".encodeToByteArray()
        val signature = keys.sign(message)
        val s = BigInteger(1, signature.copyOfRange(32, 64).reversedArray()) + order
        val bytes = s.toByteArray().reversedArray().copyOf(32)
        val malleated = signature.copyOfRange(0, 32) + bytes

        assertTrue(Ed25519.verify(keys.publicKey, message, signature))
        assertFalse(Ed25519.verify(keys.publicKey, message, malleated))
    }

    @Test
    fun `keys and signatures of the wrong length, and keys on no point, are refused`() {
        val keys = TestKeys()
        val message = byteArrayOf(1, 2, 3)
        val signature = keys.sign(message)
        assertFalse(Ed25519.verify(keys.publicKey.copyOf(31), message, signature))
        assertFalse(Ed25519.verify(keys.publicKey, message, signature.copyOf(63)))
        // y = 2 is on no point of the curve.
        assertFalse(Ed25519.verify(ByteArray(32).also { it[0] = 2 }, message, signature))
    }

    @Test
    fun `a key of small order is refused`() {
        // The neutral point as a key, with the neutral point and zero as the signature, satisfies the
        // equation for every message. The agent's library takes it; Bouncy Castle refuses such a key.
        val neutral = ByteArray(32).also { it[0] = 1 }
        val signature = neutral + ByteArray(32)
        assertFalse(Ed25519.verify(neutral, "anything".encodeToByteArray(), signature))
        // The same point written with y = p + 1 instead of 1.
        val unreduced = ByteArray(32) { 0xFF.toByte() }.also { it[0] = 0xEE.toByte(); it[31] = 0x7F }
        assertFalse(Ed25519.verify(unreduced, "anything".encodeToByteArray(), signature))
    }

    @Test
    fun `shows a key's fingerprint as the agent repo's key files do`() {
        assertEquals("8930 3d77 2cec c26d", Ed25519.fingerprint(AgentVectors.packageKey))
        assertEquals("187e 452f 18b1 aa7c", Ed25519.fingerprint(AgentVectors.indexKey))
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.ssh

import com.nbtapps.store.data.crypto.Ed25519
import com.nbtapps.store.data.crypto.Hex
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/** Keeps the app's SSH key in memory, for tests: the Android Keystore that [KeystoreSshIdentity] needs is not on the JVM. */
class MemorySshIdentity(private val key: SshKey = SshKey.generate()) : SshIdentity {
    override suspend fun key(): SshKey = key
}

/**
 * The app's SSH key ([SshKey]): made and used with Bouncy Castle, it signs as RFC 8032 has it, and its public line is
 * one the relay takes. The key the app ships with is kept by [KeystoreSshIdentity], which a JVM test cannot reach.
 */
class SshIdentityTest {

    @Test
    fun `a key made from a seed is RFC 8032's, and signs as it says`() {
        // RFC 8032, section 7.1, test 1: the empty message.
        val key = SshKey.fromSeed(Hex.decodeOrNull("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", 32)!!)
        assertEquals("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", Hex.encode(key.publicKey))
        assertEquals(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
            Hex.encode(key.sign(ByteArray(0))),
        )
    }

    @Test
    fun `its public line passes the relay's rule and holds its blob, and an SSH signature wraps the plain one`() {
        val key = SshKey.generate()
        val line = key.publicKeyLine()
        assertTrue(line, SshKeyLines.passes(line))
        assertEquals("nbtapps", SshKeyLines.comment(line))
        assertArrayEquals(key.blob, SshKeyLines.blob(line))
        assertArrayEquals(SshKeyLines.sshStrings("ssh-ed25519".encodeToByteArray(), key.publicKey), key.blob)
        assertEquals(SshKeyLines.fingerprint(key.blob), key.fingerprint)

        val data = "session id and the rest".encodeToByteArray()
        val signature = key.sshSignature(data)
        assertArrayEquals(SshKeyLines.sshStrings("ssh-ed25519".encodeToByteArray(), key.sign(data)), signature)
        assertTrue(Ed25519.verify(key.publicKey, data, key.sign(data)))
        assertFalse(Ed25519.verify(key.publicKey, data + 1, key.sign(data)))
    }

    @Test
    fun `each key made is a new one, and the same seed makes the same key`() = runTest {
        val one = SshKey.generate()
        val two = SshKey.generate()
        assertNotEquals(Hex.encode(one.publicKey), Hex.encode(two.publicKey))
        assertArrayEquals(one.publicKey, SshKey.fromSeed(one.seed()).publicKey)

        val identity = MemorySshIdentity(one)
        assertSame(one, identity.key())
        assertEquals(one.publicKeyLine(), identity.publicKeyLine())
    }
}

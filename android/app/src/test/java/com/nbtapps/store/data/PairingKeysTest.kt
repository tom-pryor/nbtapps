// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.SerialEndpoint
import com.nbtapps.store.model.TcpEndpoint
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What goes with a head unit when it is removed, and the stand-in key store the other tests use. The store the
 * app ships with is the Android Keystore's, which a JVM test cannot reach.
 */
class PairingKeysTest {

    private val headUnits = InMemoryHeadUnitRepository()
    private val keys = MemoryPairingKeys()
    private val agentId = "000102030405060708090a0b0c0d0e0f"
    private val otherId = "f0e0d0c0b0a090807060504030201000"
    private val key = ByteArray(32) { it.toByte() }

    private fun unit(id: String, agent: String?) = HeadUnit(id, id, listOf(TcpEndpoint("192.168.0.1", 7144)), headUnitId = agent)

    @Test
    fun `the stand-in keeps a key per head unit and gives only its HMAC`() = runTest {
        assertFalse(keys.has(agentId))
        assertNull(keys.mac(agentId, byteArrayOf(1)))

        val mine = key.copyOf()
        keys.put(agentId, mine)
        // The caller wipes its copy once the key is kept.
        mine.fill(0)

        assertTrue(keys.has(agentId))
        assertFalse(keys.has(otherId))
        // The key that was put, not the wiped copy: the same bytes as the plain HMAC under it.
        assertArrayEquals(hmacSha256(key, "nbt".encodeToByteArray()), keys.mac(agentId, "nbt".encodeToByteArray()))
        // And the plain HMAC is HMAC-SHA256: RFC 4231, test case 2.
        assertEquals(
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            Hex.encode(hmacSha256("Jefe".encodeToByteArray(), "what do ya want for nothing?".encodeToByteArray())),
        )
        assertEquals(32, keys.mac(agentId, ByteArray(0))?.size)

        keys.remove(agentId)
        keys.remove(agentId)
        assertFalse(keys.has(agentId))
    }

    @Test
    fun `removing a head unit removes the key its agent issued`() = runTest {
        headUnits.add(unit("car", agentId))
        headUnits.add(unit("van", otherId))
        keys.put(agentId, key)
        keys.put(otherId, key)

        headUnits.removeWithKey("car", keys)

        assertEquals(listOf("van"), headUnits.headUnits.first().map { it.id })
        assertEquals(setOf(otherId), keys.headUnitIds)
    }

    @Test
    fun `the key stays while another saved entry is the same head unit`() = runTest {
        // One car saved twice: by its address on the bench, and over Bluetooth.
        headUnits.add(unit("car", agentId))
        headUnits.add(HeadUnit("car-bt", "Car", listOf(SerialEndpoint("00:11:22:33:44:55", "BMW")), headUnitId = agentId))
        keys.put(agentId, key)

        headUnits.removeWithKey("car", keys)
        assertEquals(setOf(agentId), keys.headUnitIds)

        headUnits.removeWithKey("car-bt", keys)
        assertEquals(emptySet<String>(), keys.headUnitIds)
    }

    @Test
    fun `a head unit never spoken to, or one not saved, takes no key with it`() = runTest {
        headUnits.add(unit("new", null))
        keys.put(agentId, key)

        headUnits.removeWithKey("new", keys)
        headUnits.removeWithKey("nobody", keys)

        assertTrue(headUnits.headUnits.first().isEmpty())
        assertEquals(setOf(agentId), keys.headUnitIds)
    }

    @Test
    fun `a key store that fails does not keep the head unit from being removed`() = runTest {
        headUnits.add(unit("car", agentId))
        keys.put(agentId, key)
        keys.broken = "The phone's key store failed"

        headUnits.removeWithKey("car", keys)

        assertTrue(headUnits.headUnits.first().isEmpty())
    }
}

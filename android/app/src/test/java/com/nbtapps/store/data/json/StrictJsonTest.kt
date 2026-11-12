// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.json

import com.nbtapps.store.AgentVectors
import java.io.ByteArrayOutputStream
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class StrictJsonTest {

    /** `%HH` stands for the byte HH; a note may follow two spaces and a #. */
    private fun decode(input: String): ByteArray {
        val text = input.substringBefore("  #")
        val out = ByteArrayOutputStream()
        var i = 0
        while (i < text.length) {
            if (text[i] == '%') {
                out.write(text.substring(i + 1, i + 3).toInt(16))
                i += 3
            } else {
                out.write(text[i].toString().encodeToByteArray())
                i++
            }
        }
        return out.toByteArray()
    }

    @Test
    fun `agrees with every case of the shared profile vectors`() {
        var ok = 0
        var bad = 0
        AgentVectors.file("json-profile.vectors").readLines().forEachIndexed { index, line ->
            if (line.isBlank() || line.startsWith("#")) return@forEachIndexed
            val accepted = try {
                StrictJson.parseObject(decode(line.substringAfter(' ', "")))
                true
            } catch (e: JsonProfileException) {
                false
            }
            when {
                line.startsWith("ok") -> {
                    if (!accepted) fail("line ${index + 1} should parse: $line")
                    ok++
                }
                line.startsWith("bad") -> {
                    if (accepted) fail("line ${index + 1} should be refused: ${line.take(120)}")
                    bad++
                }
                else -> fail("line ${index + 1} is neither ok nor bad")
            }
        }
        assertTrue("the vectors file looks cut short: $ok ok, $bad bad", ok >= 20 && bad >= 80)
    }

    @Test
    fun `values come out as written`() {
        val root = StrictJson.parseObject(
            """{"n":-12,"big":9007199254740991,"s":"aé😀\n","t":true,"z":null,"a":[1,{"k":"v"}],"o":{}}"""
                .encodeToByteArray(),
        )
        assertEquals(-12L, root.getValue("n").jsonPrimitive.long)
        assertEquals(9007199254740991L, root.getValue("big").jsonPrimitive.long)
        assertEquals("aé😀\n", root.getValue("s").jsonPrimitive.content)
        assertEquals(JsonPrimitive(true), root.getValue("t"))
        assertEquals(JsonNull, root.getValue("z"))
        assertEquals("v", root.getValue("a").jsonArray[1].jsonObject.getValue("k").jsonPrimitive.content)
        assertEquals(listOf("n", "big", "s", "t", "z", "a", "o"), root.keys.toList())
    }
}

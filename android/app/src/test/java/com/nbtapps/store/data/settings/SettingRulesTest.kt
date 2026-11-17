// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.settings

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.json.StrictJson
import com.nbtapps.store.data.link.AgentClient
import com.nbtapps.store.data.packages.ManifestParser
import com.nbtapps.store.model.SettingOption
import com.nbtapps.store.model.SettingValue
import java.io.ByteArrayOutputStream
import kotlinx.serialization.json.JsonElement
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The phone's checks of a setting's value, held to the agent's: every `write` line of `spec/settings.vectors`, and
 * what the screen makes of what the user types.
 */
class SettingRulesTest {

    private val lines = AgentVectors.file("settings.vectors").readLines().filter { it.isNotEmpty() && !it.startsWith("#") }

    /** The options of the file, read as a manifest of their own is read. */
    private val options: Map<String, SettingOption> by lazy {
        val texts = lines.filter { it.startsWith("option ") }.map { it.removePrefix("option ") }
        val manifest = """{"manifestVersion":1,"id":"org.nbtapp.vectors","name":"Vectors","version":"1.0.0","contents":[],""" +
            """"settings":{"options":[${texts.joinToString(",")}]}}"""
        requireNotNull(ManifestParser.parse(manifest.encodeToByteArray()).settings).options.associateBy { it.key }
    }

    private fun json(text: String): JsonElement? = StrictJson.parseObject("""{"v":$text}""".encodeToByteArray())["v"]

    /** TEXT of the file: bytes from ! to ~ as themselves but for %, every other as %HH; a bare = is no bytes. */
    private fun bytes(text: String): ByteArray {
        val out = ByteArrayOutputStream()
        var i = 0
        while (i < text.length) {
            if (text[i] == '%') {
                out.write(text.substring(i + 1, i + 3).toInt(16))
                i += 3
            } else {
                out.write(text[i].code)
                i++
            }
        }
        return out.toByteArray()
    }

    @Test
    fun `every write line of the vectors is refused for its reason, or written as its line`() {
        val writes = lines.filter { it.startsWith("write ") }
        assertTrue("the file has its write lines", writes.size >= 40)
        for (line in writes) {
            val (_, key, outcome, value) = line.split(" ", limit = 4)
            val option = requireNotNull(options[key]) { "no option $key" }
            val element = json(value)
            val reason = SettingRules.checkJson(option, element)
            if (outcome.startsWith("=")) {
                assertNull(line, reason)
                val written = SettingRules.line(requireNotNull(AgentClient.settingValue(option.type, element)))
                assertArrayEquals(line, bytes(outcome.substring(1)), written.encodeToByteArray())
            } else {
                assertEquals(line, outcome, reason)
            }
        }
    }

    @Test
    fun `what is typed for a number is held to the same rules`() {
        val n = options.getValue("N")
        assertEquals(SettingCheck.Ok(SettingValue.Number(7)), SettingRules.check(n, "007"))
        assertEquals(SettingCheck.Ok(SettingValue.Number(-5)), SettingRules.check(n, "-5"))
        assertEquals(SettingCheck.Refused(SettingRules.OUT_OF_RANGE), SettingRules.check(n, "101"))
        assertEquals(SettingCheck.Refused(SettingRules.OUT_OF_RANGE), SettingRules.check(n, "99999999999999999999999999"))
        for (input in listOf("", "-", "+5", "5.0", "1e2", " 5", "0x10")) {
            assertEquals(input, SettingCheck.Refused(SettingRules.WRONG_TYPE), SettingRules.check(n, input))
        }
        assertEquals("Use a number from -5 to 100", SettingRules.explain(n, SettingRules.OUT_OF_RANGE))
    }

    @Test
    fun `a switch, a text and a pick are held to them too`() {
        val b = options.getValue("B")
        assertEquals(SettingCheck.Ok(SettingValue.Bool(true)), SettingRules.check(b, SettingRules.ON))
        assertEquals(SettingCheck.Ok(SettingValue.Bool(false)), SettingRules.check(b, SettingRules.OFF))
        assertEquals(SettingCheck.Refused(SettingRules.WRONG_TYPE), SettingRules.check(b, "on"))

        val t = options.getValue("T")
        assertEquals(SettingCheck.Ok(SettingValue.Text("abc-1.2")), SettingRules.check(t, "abc-1.2"))
        assertEquals(SettingCheck.Refused(SettingRules.BAD_CHARACTERS), SettingRules.check(t, ""))
        assertEquals("Enter some text", SettingRules.explain(t, SettingRules.BAD_CHARACTERS, ""))
        assertEquals(SettingCheck.Refused(SettingRules.TOO_LONG), SettingRules.check(t, "a b c d e"))
        val any = options.getValue("TA")
        // Three characters of three bytes each are more than its eight bytes.
        assertEquals(SettingCheck.Refused(SettingRules.TOO_LONG), SettingRules.check(any, "€€€"))
        assertEquals(SettingCheck.Refused(SettingRules.BAD_CHARACTERS), SettingRules.check(any, "\uD800ab"))
        assertEquals(SettingCheck.Refused(SettingRules.BAD_CHARACTERS), SettingRules.check(any, " a"))

        val s = options.getValue("S")
        assertEquals(SettingCheck.Ok(SettingValue.Text("Square.2")), SettingRules.check(s, "Square.2"))
        assertEquals(SettingCheck.Refused(SettingRules.NOT_A_CHOICE), SettingRules.check(s, "Round"))
    }

    @Test
    fun `a value's input is what the screen gives back for it`() {
        for ((option, value) in listOf(
            options.getValue("B") to SettingValue.Bool(false),
            options.getValue("WIDE") to SettingValue.Number(-2147483648),
            options.getValue("TA") to SettingValue.Text("été"),
            options.getValue("S") to SettingValue.Text("round"),
        )) {
            assertEquals(SettingCheck.Ok(value), SettingRules.check(option, SettingRules.input(value)))
        }
    }
}

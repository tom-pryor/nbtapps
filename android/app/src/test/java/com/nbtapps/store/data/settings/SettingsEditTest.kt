// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.settings

import com.nbtapps.store.model.SettingChoice
import com.nbtapps.store.model.SettingEntry
import com.nbtapps.store.model.SettingOption
import com.nbtapps.store.model.SettingState
import com.nbtapps.store.model.SettingType
import com.nbtapps.store.model.SettingValue
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The changes a screen keeps before they are saved, and how they are cut to fit the link's frames. */
class SettingsEditTest {

    private val chime = SettingOption("CHIME", "Chime", SettingType.BOOL, default = SettingValue.Bool(true))
    private val speed = SettingOption("DIAL_SPEED", "Dial speed", SettingType.INT, min = 1, max = 50, default = SettingValue.Number(10))
    private val host = SettingOption("LAMP_HOST", "Lamp address", SettingType.TEXT, maxLength = 64, unset = "No lamp")
    private val face = SettingOption(
        "FACE", "Dial face", SettingType.SELECT,
        values = listOf(SettingChoice("round", "Round"), SettingChoice("hex", "Hexagon")), default = SettingValue.Text("round"),
    )

    private val read = SettingsEdit(
        listOf(
            SettingEntry(chime, SettingState.UNSET),
            SettingEntry(speed, SettingState.SET, value = SettingValue.Number(25)),
            SettingEntry(host, SettingState.UNSET),
            SettingEntry(face, SettingState.BY_HAND, raw = "oval"),
        ),
    )

    @Test
    fun `a change that brings an option back to how it was read is no change`() {
        val off = read.set("CHIME", SettingRules.OFF)
        assertEquals(mapOf("CHIME" to "false"), off.inputs)
        // Back on: the default it was going by when it was read.
        assertEquals(0, off.set("CHIME", SettingRules.ON).count)
        assertEquals(1, read.set("DIAL_SPEED", "30").count)
        assertEquals(0, read.set("DIAL_SPEED", "30").set("DIAL_SPEED", "25").count)
        assertEquals(0, read.set("LAMP_HOST", "").count)
        // Set by hand: whatever is picked is a change, but the raw text itself is none.
        assertEquals(1, read.set("FACE", "round").count)
        assertEquals(0, read.set("FACE", "oval").count)
        // Unknown options are left alone.
        assertEquals(read, read.set("NOPE", "1").reset("NOPE"))
    }

    @Test
    fun `a field keeps what was typed, and an input that comes to the default of an option not set is no change`() {
        val greeting = SettingOption("GREETING", "Greeting", SettingType.TEXT, maxLength = 40, anyCharacters = true, default = SettingValue.Text("Hello"))
        val unset = SettingsEdit(listOf(SettingEntry(speed, SettingState.UNSET), SettingEntry(greeting, SettingState.UNSET)))

        // Typed one key at a time over the default 10.
        var edit = unset.set("DIAL_SPEED", "1")
        assertEquals("1", edit.inputs["DIAL_SPEED"])
        assertEquals(1, edit.count)
        edit = edit.set("DIAL_SPEED", "10")
        assertEquals("the field still holds it", "10", edit.inputs["DIAL_SPEED"])
        assertEquals("but it is the default: no change", 0, edit.count)
        assertFalse(edit.isChange("DIAL_SPEED"))
        assertEquals(SettingsChanges(), edit.changes())
        edit = edit.set("DIAL_SPEED", "100")
        assertEquals("100", edit.inputs["DIAL_SPEED"])
        assertEquals(setOf("DIAL_SPEED"), edit.changed)
        assertEquals(mapOf("DIAL_SPEED" to SettingRules.OUT_OF_RANGE), edit.problems)

        // Text that starts with the default.
        edit = unset.set("GREETING", "Hell").set("GREETING", "Hello")
        assertEquals("Hello", edit.inputs["GREETING"])
        assertEquals(0, edit.count)
        edit = edit.set("GREETING", "Hello!")
        assertEquals("Hello!", edit.inputs["GREETING"])
        assertEquals(SettingsChanges(mapOf("GREETING" to SettingValue.Text("Hello!"))), edit.changes())
        // Emptied, the field shows nothing of its own again.
        assertTrue(edit.set("GREETING", "").inputs.isEmpty())

        // A set value typed with a leading zero is the same value: the field keeps it, and it is no change.
        val zero = read.set("DIAL_SPEED", "025")
        assertEquals("025", zero.inputs["DIAL_SPEED"])
        assertEquals(0, zero.count)
    }

    @Test
    fun `a switch of an option not set with no default, switched on and off again, is no change`() {
        val lamp = SettingOption("LAMP", "Lamp", SettingType.BOOL)
        val unset = SettingsEdit(listOf(SettingEntry(lamp, SettingState.UNSET)))
        val on = unset.set("LAMP", SettingRules.ON)
        assertEquals(1, on.count)
        val off = on.set("LAMP", SettingRules.OFF)
        assertEquals(0, off.count)
        assertEquals(unset, off)
        assertEquals(SettingsChanges(), off.changes())
    }

    @Test
    fun `a reset takes a set option, or one set by hand, back to its default, and one not set stays as it is`() {
        val reset = read.reset("DIAL_SPEED").reset("FACE").reset("CHIME")
        assertEquals(setOf("DIAL_SPEED", "FACE"), reset.resets)
        assertTrue(reset.inputs.isEmpty())
        // A reset drops a change of the same option, and a change drops a reset.
        assertEquals(emptyMap<String, String>(), read.set("CHIME", "false").reset("CHIME").inputs)
        val again = reset.set("DIAL_SPEED", "40")
        assertEquals(setOf("FACE"), again.resets)
        assertEquals(mapOf("DIAL_SPEED" to "40"), again.inputs)
        assertEquals(setOf("DIAL_SPEED", "FACE"), again.changed)
        assertEquals(0, again.discard().count)
    }

    @Test
    fun `once saved, the options are as saved, and a change made while the save went on stays`() {
        val edit = read.set("CHIME", SettingRules.OFF).set("DIAL_SPEED", "30").set("LAMP_HOST", "lamp.local").reset("FACE")
        val sent = requireNotNull(edit.changes())
        // While it was saved, the speed was changed again.
        val after = edit.set("DIAL_SPEED", "35").saved(sent)
        assertEquals(SettingEntry(chime, SettingState.SET, value = SettingValue.Bool(false)), after.entry("CHIME"))
        assertEquals(SettingEntry(speed, SettingState.SET, value = SettingValue.Number(30)), after.entry("DIAL_SPEED"))
        assertEquals(SettingEntry(host, SettingState.SET, value = SettingValue.Text("lamp.local")), after.entry("LAMP_HOST"))
        assertEquals(SettingEntry(face, SettingState.UNSET), after.entry("FACE"))
        assertEquals(mapOf("DIAL_SPEED" to "35"), after.inputs)
        assertEquals(setOf("DIAL_SPEED"), after.changed)
        assertEquals(SettingsChanges(mapOf("DIAL_SPEED" to SettingValue.Number(35))), after.changes())

        // Reset while it was saved: the value saved goes again.
        assertEquals(setOf("DIAL_SPEED"), edit.reset("DIAL_SPEED").saved(sent).resets)

        // Only some saved, as a save in parts that failed part way: the rest stay as read, and still changes.
        val part = edit.saved(sent, setOf("CHIME"))
        assertEquals(SettingValue.Bool(false), part.entry("CHIME")?.value)
        assertEquals(SettingValue.Number(25), part.entry("DIAL_SPEED")?.value)
        assertEquals(setOf("DIAL_SPEED", "LAMP_HOST", "FACE"), part.changed)
    }

    @Test
    fun `changes go out in the order of the options, and not while an input would be refused`() {
        val edit = read.set("LAMP_HOST", "lamp.local").reset("FACE").set("CHIME", "false").set("DIAL_SPEED", "51")
        assertEquals(mapOf("DIAL_SPEED" to SettingRules.OUT_OF_RANGE), edit.problems)
        assertNull(edit.changes())
        val fixed = edit.set("DIAL_SPEED", "50")
        assertTrue(fixed.problems.isEmpty())
        val changes = requireNotNull(fixed.changes())
        assertEquals(listOf("CHIME", "DIAL_SPEED", "LAMP_HOST"), changes.set.keys.toList())
        assertEquals(SettingValue.Number(50), changes.set["DIAL_SPEED"])
        assertEquals(listOf("FACE"), changes.reset)
    }

    @Test
    fun `changes are kept over the settings read again`() {
        val edit = read.set("DIAL_SPEED", "30").set("LAMP_HOST", "lamp.local").reset("FACE")
        // Read again: the speed is 30 now anyway, and the face is gone from the manifest.
        val fresh = listOf(
            SettingEntry(chime, SettingState.UNSET),
            SettingEntry(speed, SettingState.SET, value = SettingValue.Number(30)),
            SettingEntry(host, SettingState.UNSET),
        )
        val over = edit.over(fresh)
        assertEquals(fresh, over.entries)
        assertEquals(mapOf("LAMP_HOST" to "lamp.local"), over.inputs)
        assertTrue(over.resets.isEmpty())
    }

    @Test
    fun `changes that fit one frame go in one request, and more are cut into requests that each fit`() {
        val small = SettingsChanges(mapOf("CHIME" to SettingValue.Bool(false)), listOf("FACE"))
        assertEquals(listOf(small), small.parts("org.nbtapp.dial", 4096))

        // Forty values of 200 bytes, each with JSON's escapes for a quote: far more than one frame of 4,096.
        val set = (1..40).associate { "KEY_$it" to SettingValue.Text("\"".repeat(100) + "x".repeat(100)) }
        val reset = (1..30).map { "GONE_$it" }
        val changes = SettingsChanges(set, reset)
        assertTrue(SettingsChanges.requestBytes("org.nbtapp.dial", changes) > 4096)
        val parts = changes.parts("org.nbtapp.dial", 4096)
        assertTrue("${parts.size} parts", parts.size in 3..15)
        for (part in parts) assertTrue(SettingsChanges.requestBytes("org.nbtapp.dial", part) <= 4096)
        // Every change once, in the order they were given, the sets before the resets.
        assertEquals(set.keys.toList(), parts.flatMap { it.set.keys })
        assertEquals(reset, parts.flatMap { it.reset })
        assertEquals(set, parts.fold(emptyMap<String, SettingValue>()) { all, part -> all + part.set })
        val firstReset = parts.indexOfFirst { it.reset.isNotEmpty() }
        assertTrue(parts.drop(firstReset + 1).all { it.set.isEmpty() })
    }
}

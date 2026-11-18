// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.json.StrictJson
import com.nbtapps.store.data.settings.FutureSettings
import com.nbtapps.store.model.PackageSettings
import com.nbtapps.store.model.SettingApplies
import com.nbtapps.store.model.SettingChoice
import com.nbtapps.store.model.SettingOption
import com.nbtapps.store.model.SettingType
import com.nbtapps.store.model.SettingValue
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * A manifest's `settings`, as [ManifestParser] reads it: the test packages the agent repo builds (`PackageCorpusTest`
 * runs the broken ones, which the agent refuses and the app takes with no settings), the edges they do not reach, and
 * the options a later agent may send ([FutureSettings]).
 */
class ManifestSettingsTest {

    private fun settings(file: String): PackageSettings? =
        PackageArchive.verify(AgentVectors.file("packages/good/$file"), listOf(AgentVectors.packageKey)).manifest.settings

    /**
     * What a manifest with this `settings` comes to: "kept" where its settings are read, "dropped" where they break the
     * agent's rules, which the agent refuses (see [ManifestParser]), or the code it is refused with.
     */
    private fun code(settings: String, install: String = ""): String = try {
        val text = """{"manifestVersion":1,"id":"org.nbtapp.t","name":"T","version":"1.0.0","settings":$settings,""" +
            """"contents":[]${if (install.isEmpty()) "" else ""","install":{$install}"""}}"""
        if (ManifestParser.parse(text.encodeToByteArray()).settings != null) "kept" else "dropped"
    } catch (e: PackageFormatException) {
        e.code
    }

    private fun lamp(members: String) = code("""{"options":[{"key":"LAMP","label":"Lamp",$members}]}""")

    @Test
    fun `the dial package's settings are read, in the order of the text, with the option of an unknown type left out`() {
        val dial = settings("org.nbtapp.dial-1.0.0.nbtpkg")!!
        assertEquals("\${data}/dial.conf", dial.file)
        assertEquals(listOf("CHIME", "DIAL_SPEED", "GREETING", "LAMP_HOST", "FACE", "LAMP_COLOUR"), dial.options.map { it.key })
        assertEquals(
            SettingOption("CHIME", "Chime", SettingType.BOOL, description = "Play a chime when the dial turns.",
                default = SettingValue.Bool(true)),
            dial.options[0],
        )
        assertEquals(
            SettingOption("DIAL_SPEED", "Dial speed", SettingType.INT, description = "How far the dial turns each second.",
                min = 1, max = 50, unit = "°/s", default = SettingValue.Number(10), applies = SettingApplies.APP_START),
            dial.options[1],
        )
        val greeting = dial.options[2]
        assertTrue(greeting.anyCharacters && greeting.maxLength == 40 && greeting.applies == SettingApplies.REBOOT)
        assertEquals(SettingValue.Text("Hello, dial é"), greeting.default)
        val host = dial.options[3]
        assertTrue(!host.anyCharacters && host.advanced && host.default == null && host.unset == "No lamp")
        val face = dial.options[4]
        assertEquals(listOf(SettingChoice("round", "Round"), SettingChoice("square", "Square"), SettingChoice("hex", "Hexagon")), face.values)
        assertEquals(SettingValue.Text("round"), face.default)
        assertEquals(SettingApplies.HMI_RESTART, face.applies)
        val colour = dial.options[5]
        assertTrue(colour.type == SettingType.SELECT && colour.advanced && colour.unset == "As the lamp has it" && colour.default == null)
        // A package that names none has none.
        assertNull(settings("org.nbtapp.hello-1.2.0.nbtpkg"))
    }

    @Test
    fun `settings in every form, in the default file, and 64 options in a file of the package's choice`() {
        val every = settings("settings.nbtpkg")!!
        assertEquals(PackageSettings.DEFAULT_FILE, every.file)
        assertEquals(
            listOf("LAMP", "CHIME", "SPEED", "STEPS", "LEVEL", "HOST", "GREETING", "NAME", "FACE", "FACE_2"),
            every.options.map { it.key },
        )
        val speed = every.options[2]
        assertEquals(listOf(-2147483648L, 2147483647L, -5L), listOf(speed.min, speed.max, (speed.default as SettingValue.Number).value))
        assertEquals(16, every.options[9].values.size)
        assertEquals("Automatic", every.options[9].unset)

        val most = settings("settings-most.nbtpkg")!!
        assertEquals("\${data}/conf/dial settings.txt", most.file)
        assertEquals(ManifestParser.MAX_SETTINGS, most.options.size)
    }

    @Test
    fun `the edges of each member`() {
        assertEquals("kept", lamp(""""type":"bool""""))
        assertEquals("kept", lamp(""""type":"colour","default":{},"unset":"","description":7"""))
        assertEquals("dropped", lamp(""""type":1"""))
        assertEquals("dropped", code("""{"options":[{"key":"lamp","label":"Lamp","type":"colour"}]}"""))
        assertEquals("dropped", code("""{"options":[{"key":"A","label":"A","type":"bool"},{"key":"A","label":"B","type":"x"}]}"""))
        assertEquals("dropped", lamp(""""type":"bool","default":true,"unset":"Off""""))
        assertEquals("kept", lamp(""""type":"int","min":5,"max":5,"default":5"""))
        assertEquals("dropped", lamp(""""type":"int","min":5,"max":4"""))
        assertEquals("dropped", lamp(""""type":"int","min":0,"max":2147483648"""))
        assertEquals("kept", lamp(""""type":"bool","min":"x","unit":"""""))   // an int's members on a bool are ignored
        assertEquals("dropped", lamp(""""type":"text","maxLength":256"""))
        assertEquals("dropped", lamp(""""type":"text","maxLength":5,"characters":"any","default":"ééé""""))
        assertEquals("kept", lamp(""""type":"text","maxLength":6,"characters":"any","default":"ééé""""))
        assertEquals("dropped", lamp(""""type":"text","maxLength":6,"characters":"latin""""))
        assertEquals("dropped", lamp(""""type":"select","values":[{"value":"a","label":"A"}]"""))
        assertEquals("dropped", lamp(""""type":"bool","applies":"later""""))
        assertEquals("dropped", code("7"))
        assertEquals("dropped", code("""{"options":[]}"""))

        // At most 1,024 bytes of UTF-8 in one option's strings: bytes, not characters.
        val accents = "é".repeat(500)
        assertEquals("kept", code("""{"options":[{"key":"LAMP","label":"${"L".repeat(20)}","type":"bool","description":"$accents"}]}"""))
        assertEquals("dropped", code("""{"options":[{"key":"LAMP","label":"${"L".repeat(21)}","type":"bool","description":"$accents"}]}"""))

        // Settings never refuse a manifest, nor does a key unknown in install, which is of a later format.
        assertEquals("dropped", code("""{"options":[{"key":"A","label":"A","type":"bool","default":1}]}""", "\"later\":1"))
        assertEquals("dropped", code("""{"options":[{"key":"a","label":"A","type":"bool"}]}""", "\"later\":1"))
        // Nor do they hide a fault elsewhere.
        assertEquals("bad-manifest", code("""{"options":[{"key":"a","label":"A","type":"bool"}]}""", "\"reboot\":1"))
        assertEquals("bad-manifest", code("""{"options":[{"key":"a","label":"A","type":"bool"}]}""", "\"later\":1,\"files\":{}"))
    }

    @Test
    fun `what a text option takes, and where the settings file may be`() {
        for (value in listOf("a", "Z9", "lamp-1.example:80/x@y+z,w_v")) {
            assertTrue(value, ManifestParser.isSettingText(value, 255, false) && ManifestParser.isSettingText(value, 255, true))
        }
        for (value in listOf("", "a b", "a=b", "a#b", "é", "a\tb", "a;b")) assertFalse(value, ManifestParser.isSettingText(value, 255, false))
        for (value in listOf("a b", "a=b;c#d", "é x", "!")) assertTrue(value, ManifestParser.isSettingText(value, 255, true))
        for (value in listOf("", " a", "a ", " ", "a\tb", "a\nb", "a\u007fb")) assertFalse(value, ManifestParser.isSettingText(value, 255, true))
        assertTrue(ManifestParser.isSettingText("éé", 4, true))
        assertFalse(ManifestParser.isSettingText("éé", 3, true))

        for (path in listOf("\${data}/a", "\${data}/a/b.conf", "\${data}/.x", "\${data}/a b", "\${data}/" + "f".repeat(247))) {
            assertTrue(path, ManifestParser.isSettingsFile(path))
        }
        for (path in listOf(
            "", "\${data}", "\${data}/", "\${data}/a/", "\${data}//a", "\${data}/../a", "\${app}/a", "\${tmp}/a", "/tmp/a",
            "a", "\${data}/a\\b", "\${data}/" + "f".repeat(248),
        )) {
            assertFalse(path, ManifestParser.isSettingsFile(path))
        }
        for (key in listOf("A", "A_1", "NBT_VID_FPS", "K".repeat(64))) assertTrue(key, ManifestParser.isSettingKey(key))
        for (key in listOf("", "a", "Lamp", "1A", "_A", "A-B", "K".repeat(65))) assertFalse(key, ManifestParser.isSettingKey(key))
    }

    private fun option(json: String): SettingOption = ManifestParser.settingOption(StrictJson.parseObject(json.encodeToByteArray()))

    @Test
    fun `an option a later agent sends is read leniently, and one this app cannot read is of no type it knows`() {
        val glow = option(FutureSettings.GLOW)
        assertEquals(
            SettingOption("GLOW", "Glow colour", SettingType.UNKNOWN, description = "The colour of the dial's glow."),
            glow,
        )
        val volume = option(FutureSettings.VOLUME)
        assertEquals(SettingType.INT, volume.type)
        assertEquals(SettingApplies.UNKNOWN, volume.applies)
        assertEquals(SettingValue.Number(5), volume.default)
        val callSign = option(FutureSettings.CALL_SIGN)
        assertTrue(callSign.type == SettingType.TEXT && callSign.unknownCharacters && !callSign.anyCharacters)
        assertEquals(1000, option(FutureSettings.BANNER).maxLength)
        val offset = option(FutureSettings.OFFSET)
        assertEquals(-10_000_000_000L..10_000_000_000L, offset.min..offset.max)
        assertEquals(20, option(FutureSettings.STATION).values.size)
        assertEquals(SettingType.BOOL, option(FutureSettings.MODE).type)
        val sound = option(FutureSettings.SOUND)
        assertEquals(SettingOption("SOUND", "Sound", SettingType.UNKNOWN), sound)

        // As far as each can be read: a key of a later form, a label that cannot be shown, advanced and applies.
        assertEquals(
            SettingOption("lamp.colour", "lamp.colour", SettingType.UNKNOWN, applies = SettingApplies.HMI_RESTART, advanced = true),
            option("""{"key":"lamp.colour","label":"","type":"bool","applies":"hmi-restart","advanced":true}"""),
        )
        assertEquals(
            SettingOption("", ManifestParser.UNREADABLE_LABEL, SettingType.UNKNOWN, applies = SettingApplies.UNKNOWN),
            option("""{"key":7,"type":"bool","applies":1,"advanced":"yes"}"""),
        )
        // Still held to the rules that are not limits: a default outside them, a value twice.
        assertEquals(SettingType.UNKNOWN, option("""{"key":"A","label":"A","type":"int","min":0,"max":9,"default":10}""").type)
        assertEquals(SettingType.UNKNOWN, option("""{"key":"A","label":"A","type":"text","maxLength":0}""").type)

        // A manifest that gives such options has its settings dropped: the app holds a manifest to today's rules.
        assertEquals("dropped", code("""{"options":[${FutureSettings.VOLUME}]}"""))
        assertEquals("dropped", code("""{"options":[${FutureSettings.BANNER}]}"""))
        assertEquals("kept", code("""{"options":[${FutureSettings.GLOW}]}"""))
    }

    @Test
    fun `the broken settings blocks the agent refuses are in the corpus`() {
        // PackageCorpusTest runs every one of them, and expects the app to take each with no settings.
        val names = AgentVectors.file("packages/vectors.json").readText()
        for (label in listOf("settings-bytes-description", "settings-bytes-select", "settings-select-too-many-values")) {
            if ("bad/$label.nbtpkg" !in names) fail("$label is not in the corpus")
        }
    }
}

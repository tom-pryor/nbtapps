// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.model.PackageSettings
import com.nbtapps.store.model.SettingApplies
import com.nbtapps.store.model.SettingChoice
import com.nbtapps.store.model.SettingDefault
import com.nbtapps.store.model.SettingOption
import com.nbtapps.store.model.SettingType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * A manifest's `settings`, as [ManifestParser] reads it: the test packages the agent repo builds, which every reader
 * must take or refuse alike (`PackageCorpusTest` runs the refused ones), and the edges they do not reach.
 */
class ManifestSettingsTest {

    private fun settings(file: String): PackageSettings? =
        PackageArchive.verify(AgentVectors.file("packages/good/$file"), listOf(AgentVectors.packageKey)).manifest.settings

    /** The code a manifest with this `settings` gets, and "" when it is taken. */
    private fun code(settings: String, install: String = ""): String = try {
        val text = """{"manifestVersion":1,"id":"org.nbtapp.t","name":"T","version":"1.0.0","settings":$settings,""" +
            """"contents":[]${if (install.isEmpty()) "" else ""","install":{$install}"""}}"""
        ManifestParser.parse(text.encodeToByteArray())
        ""
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
                default = SettingDefault.Bool(true)),
            dial.options[0],
        )
        assertEquals(
            SettingOption("DIAL_SPEED", "Dial speed", SettingType.INT, description = "How far the dial turns each second.",
                min = 1, max = 50, unit = "°/s", default = SettingDefault.Number(10), applies = SettingApplies.APP_START),
            dial.options[1],
        )
        val greeting = dial.options[2]
        assertTrue(greeting.anyCharacters && greeting.maxLength == 40 && greeting.applies == SettingApplies.REBOOT)
        assertEquals(SettingDefault.Text("Hello, dial é"), greeting.default)
        val host = dial.options[3]
        assertTrue(!host.anyCharacters && host.advanced && host.default == null && host.unset == "No lamp")
        val face = dial.options[4]
        assertEquals(listOf(SettingChoice("round", "Round"), SettingChoice("square", "Square"), SettingChoice("hex", "Hexagon")), face.values)
        assertEquals(SettingDefault.Text("round"), face.default)
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
        assertEquals(listOf(-2147483648L, 2147483647L, -5L), listOf(speed.min, speed.max, (speed.default as SettingDefault.Number).value))
        assertEquals(16, every.options[9].values.size)
        assertEquals("Automatic", every.options[9].unset)

        val most = settings("settings-most.nbtpkg")!!
        assertEquals("\${data}/conf/dial settings.txt", most.file)
        assertEquals(ManifestParser.MAX_SETTINGS, most.options.size)
    }

    @Test
    fun `the edges of each member`() {
        assertEquals("", lamp(""""type":"bool""""))
        assertEquals("", lamp(""""type":"colour","default":{},"unset":"","description":7"""))
        assertEquals("bad-manifest", lamp(""""type":1"""))
        assertEquals("bad-manifest", code("""{"options":[{"key":"lamp","label":"Lamp","type":"colour"}]}"""))
        assertEquals("bad-manifest", code("""{"options":[{"key":"A","label":"A","type":"bool"},{"key":"A","label":"B","type":"x"}]}"""))
        assertEquals("bad-manifest", lamp(""""type":"bool","default":true,"unset":"Off""""))
        assertEquals("", lamp(""""type":"int","min":5,"max":5,"default":5"""))
        assertEquals("bad-manifest", lamp(""""type":"int","min":5,"max":4"""))
        assertEquals("bad-manifest", lamp(""""type":"int","min":0,"max":2147483648"""))
        assertEquals("", lamp(""""type":"bool","min":"x","unit":"""""))   // an int's members on a bool are ignored
        assertEquals("bad-manifest", lamp(""""type":"text","maxLength":256"""))
        assertEquals("bad-manifest", lamp(""""type":"text","maxLength":5,"characters":"any","default":"ééé""""))
        assertEquals("", lamp(""""type":"text","maxLength":6,"characters":"any","default":"ééé""""))
        assertEquals("bad-manifest", lamp(""""type":"select","values":[{"value":"a","label":"A"}]"""))
        assertEquals("bad-manifest", lamp(""""type":"bool","applies":"later""""))

        // At most 1,024 bytes of UTF-8 in one option's strings: bytes, not characters.
        val accents = "é".repeat(500)
        assertEquals("", code("""{"options":[{"key":"LAMP","label":"${"L".repeat(20)}","type":"bool","description":"$accents"}]}"""))
        assertEquals("bad-manifest", code("""{"options":[{"key":"LAMP","label":"${"L".repeat(21)}","type":"bool","description":"$accents"}]}"""))

        // The rules of an option's type come after the walk: a key unknown in install, later in the text, decides.
        assertEquals("unsupported-manifest", code("""{"options":[{"key":"A","label":"A","type":"bool","default":1}]}""", "\"later\":1"))
        assertEquals("bad-manifest", code("""{"options":[{"key":"a","label":"A","type":"bool"}]}""", "\"later\":1"))
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

    @Test
    fun `a broken settings block is refused as the agent refuses it`() {
        // PackageCorpusTest runs every refused package; this only makes sure the settings ones are there.
        val names = AgentVectors.file("packages/vectors.json").readText()
        for (label in listOf("settings-bytes-description", "settings-bytes-select", "settings-select-too-many-values")) {
            if ("bad/$label.nbtpkg" !in names) fail("$label is not in the corpus")
        }
    }
}

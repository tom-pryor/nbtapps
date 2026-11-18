// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.model.PackageManifest
import com.nbtapps.store.model.PackageRequirements
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What [ManifestParser] takes in `install` and `requires` that this app does not know: it is skipped, the manifest is
 * marked as of a later format, and the risk flags come from what the app could read. And what it still refuses, as
 * the agent does. `PackageCorpusTest` runs the test packages the agent repo marks for this.
 */
class ManifestLaterFormatTest {

    private val hash = "0a".repeat(32)

    /** A manifest with this `install` and `requires`, whose contents list `payload/a`, `payload/b` and `scripts/s.sh`. */
    private fun text(install: String, requires: String = """{"agent":">=1.0.0"}"""): String =
        """{"manifestVersion":1,"id":"org.nbtapp.t","name":"T","version":"1.0.0","requires":$requires,""" +
            """"contents":[{"path":"payload/a","size":1,"sha256":"$hash"},{"path":"payload/b","size":1,"sha256":"$hash"},""" +
            """{"path":"scripts/s.sh","size":1,"sha256":"$hash"}],"install":{$install}}"""

    private fun parse(install: String, requires: String = """{"agent":">=1.0.0"}"""): PackageManifest =
        ManifestParser.parse(text(install, requires).encodeToByteArray())

    /** The code this manifest is refused with, or "taken". */
    private fun code(install: String, requires: String = """{"agent":">=1.0.0"}"""): String = try {
        parse(install, requires)
        "taken"
    } catch (e: PackageFormatException) {
        e.code
    }

    private fun file(dest: String, src: String = "payload/a", extra: String = "") =
        """"files":[{"src":"$src","dest":"$dest","mode":"0644"$extra}]"""

    private fun link(path: String, extra: String = "") = """"links":[{"path":"$path","target":"a"$extra}]"""

    private val hook = """{"script":"scripts/s.sh","timeout":30}"""

    @Test
    fun `a manifest of the format this app knows is not marked`() {
        val known = parse(
            file("\${app}/a") + "," + link("\${var_plugins}/A/l") + ""","hooks":{"preInstall":$hook},"reboot":"hmi-restart",""" +
                """"serialServices":[{"name":"R","program":"r"}]""",
            """{"agent":">=1.6.0","app":">=0.8.0"}""",
        )
        assertFalse(known.laterFormat)
        assertEquals(listOf("runs-scripts", "needs-reboot"), known.flags)
    }

    @Test
    fun `a folder variable this app does not know, in a destination or a link's path, is of a later format`() {
        for (install in listOf(
            file("\${home}/bin/a"),
            file("\${home}/Hello/", src = "payload/"),
            file("\${home}/", src = "payload/"),
            file("\${var_2}/a"),
            link("\${home}/l"),
            link("\${tmp}/l"),
        )) {
            val manifest = parse(install)
            assertTrue(install, manifest.laterFormat)
            // Not counted as a system file: where it is, is the agent's to say.
            assertEquals(install, emptyList<String>(), manifest.flags)
        }
        assertEquals("\${home}/bin/a", parse(file("\${home}/bin/a")).files.single().dest)
        assertEquals("\${home}/l", parse(link("\${home}/l")).links.single().path)
        // A known folder keeps what it had: an absolute path is a system file, a package's own place is not.
        assertEquals(listOf("modifies-system"), parse(file("\${home}/a") + "," + link("/usr/bin/l")).flags)
        assertEquals(listOf("modifies-system"), parse(file("\${home}/a", extra = ""","replaces":{"sha256":["$hash"]}""")).flags)
        assertFalse(parse(file("\${plugins}/A/a")).laterFormat)
        // The rest of the path is held to the rules all the same, and so is the variable's shape.
        for (dest in listOf("\${home}/../a", "\${home}//a", "\${home}", "\${home}x/a", "\${Home}/a", "\${}/a", "\${home/a", "home/a", "\${home}/a\\\\b")) {
            assertEquals(dest, PackageFormatException.BAD_MANIFEST, code(file(dest)))
        }
        for (path in listOf("\${home}", "\${home}/", "\${home}/l/", "\${home}/../l", "\${HOME}/l")) {
            assertEquals(path, PackageFormatException.BAD_MANIFEST, code(link(path)))
        }
        // A file to a folder, and one path placed twice, are refused there as anywhere.
        assertEquals(PackageFormatException.BAD_MANIFEST, code(file("\${home}/Hello/")))
        assertEquals(
            PackageFormatException.BAD_MANIFEST,
            code(""""files":[{"src":"payload/a","dest":"${'$'}{home}/x","mode":"0644"},{"src":"payload/b","dest":"${'$'}{home}/x","mode":"0644"}]"""),
        )
        assertEquals(PackageFormatException.BAD_MANIFEST, code(file("\${home}/x") + "," + link("\${home}/x")))
    }

    @Test
    fun `a key this app does not know, in install, a file, replaces, a link, a hook or a serial service, is skipped`() {
        val replaces = ""","replaces":{"sha256":["$hash"],"size":1}"""
        for ((install, flags) in listOf(
            file("\${app}/a") + ""","services":[]""" to emptyList(),
            file("\${app}/a", extra = ""","patch":"bsdiff"""") to emptyList(),
            file("\${data}/a", extra = replaces) to listOf("modifies-system"),
            link("\${app}/l", extra = ""","owner":"root"""") to emptyList(),
            """"hooks":{"postInstall":{"script":"scripts/s.sh","timeout":30,"user":"root"}}""" to listOf("runs-scripts"),
            """"serialServices":[{"name":"R","program":"r","uuid":"x"}]""" to emptyList(),
        )) {
            val manifest = parse(install)
            assertTrue(install, manifest.laterFormat)
            assertEquals(install, flags, manifest.flags)
        }
        assertEquals(listOf(hash), parse(file("\${data}/a", extra = replaces)).files.single().replaces)
        assertEquals(listOf("R"), parse(""""serialServices":[{"name":"R","program":"r","uuid":"x"}]""").serialServices.map { it.name })
        // A known member beside it is still held to its rules, before or after it.
        assertEquals(PackageFormatException.BAD_MANIFEST, code(file("\${app}/a", extra = ""","patch":1,"config":"yes"""")))
        assertEquals(PackageFormatException.BAD_MANIFEST, code(""""later":1,"files":[{"src":"payload/a","dest":"${'$'}{app}/a","mode":"644"}]"""))
        assertEquals(PackageFormatException.BAD_MANIFEST, code(file("\${app}/a", extra = ""","replaces":{"size":1}""")))
        assertEquals(PackageFormatException.BAD_MANIFEST, code(""""serialServices":[{"name":"SPP","program":"r","uuid":"x"}]"""))
        assertEquals(PackageFormatException.BAD_MANIFEST, code(""""hooks":{"preRemove":{"script":"scripts/s.sh","user":"root"}}"""))
    }

    @Test
    fun `a reboot value this app does not know is of a later format, and asks for no restart here`() {
        val later = parse(""""reboot":"after-update"""")
        assertTrue(later.laterFormat)
        assertEquals(emptyList<String>(), later.flags)
        assertEquals(listOf("needs-reboot"), parse(""""reboot":"required"""").flags)
        assertEquals(emptyList<String>(), parse(""""reboot":"none"""").flags)
        assertFalse(parse(""""reboot":"none"""").laterFormat)
        // A value that is no string is refused, as before.
        for (value in listOf("1", "null", "true", "[]", """{"when":"later"}""")) {
            assertEquals(value, PackageFormatException.BAD_MANIFEST, code(""""reboot":$value"""))
        }
    }

    @Test
    fun `a hook of a name this app does not know is not read, and runs no script as far as the flags go`() {
        for (onBoot in listOf(hook, """{"script":"scripts/none.sh"}""", "5", "[]")) {
            val manifest = parse(""""hooks":{"onBoot":$onBoot}""")
            assertTrue(onBoot, manifest.laterFormat)
            assertTrue(onBoot, manifest.hooks.isEmpty())
            assertEquals(onBoot, emptyList<String>(), manifest.flags)
        }
        val both = parse(""""hooks":{"onBoot":$hook,"preInstall":$hook}""")
        assertTrue(both.laterFormat)
        assertEquals(setOf("preInstall"), both.hooks.keys)
        assertEquals(listOf("runs-scripts"), both.flags)
        // A known hook is held to its rules as before: its script is in contents, and hooks is an object.
        assertEquals(PackageFormatException.BAD_MANIFEST, code(""""hooks":{"onBoot":$hook,"preInstall":{"script":"scripts/none.sh","timeout":1}}"""))
        assertEquals(PackageFormatException.BAD_MANIFEST, code(""""hooks":{"preInstall":{"script":"scripts/s.sh","timeout":0}}"""))
        assertEquals(PackageFormatException.BAD_MANIFEST, code(""""hooks":[]"""))
    }

    @Test
    fun `a requirement this app does not know is the agent's to judge`() {
        val later = parse(file("\${app}/a"), """{"agent":">=1.6.0","firmware":">=1.0.0","phone":{"any":[1]}}""")
        assertTrue(later.laterFormat)
        assertEquals(PackageRequirements(agent = ">=1.6.0"), later.requires)
        // The ranges this app knows are held to their form.
        for (requires in listOf("""{"agent":">=1.0"}""", """{"app":">=0.8"}""", """{"app":8}""", """{"agent":">= 1.0.0"}""", "[]", "\"x\"")) {
            assertEquals(requires, PackageFormatException.BAD_MANIFEST, code(file("\${app}/a"), requires))
        }
    }

    @Test
    fun `what the app relies on is refused as before`() {
        fun manifest(text: String) = try {
            ManifestParser.parse(text.encodeToByteArray())
            "taken"
        } catch (e: PackageFormatException) {
            e.code
        }
        val good = text(file("\${home}/a"))
        assertEquals("taken", manifest(good))
        val unsupported = PackageFormatException.UNSUPPORTED_MANIFEST
        val bad = PackageFormatException.BAD_MANIFEST
        assertEquals(unsupported, manifest(good.replace("\"manifestVersion\":1", "\"manifestVersion\":2")))
        assertEquals(bad, manifest(good.replace("\"org.nbtapp.t\"", "\"Org.T\"")))
        assertEquals(bad, manifest(good.replace("\"name\":\"T\"", "\"name\":\"\"")))
        assertEquals(bad, manifest(good.replace("\"version\":\"1.0.0\"", "\"version\":\"1.0\"")))
        assertEquals(bad, manifest(good.replace(hash, hash.uppercase())))
        assertEquals(bad, manifest(good.replace("\"requires\"", "\"depends\":{},\"requires\"")))
        assertEquals(bad, manifest(good.replace("\"requires\"", "\"conflicts\":[{\"id\":\"a.b\",\"version\":\"1\"}],\"requires\"")))
        assertEquals(bad, manifest(good.replace("\"payload/a\",\"size\"", "\"payload/../a\",\"size\"")))
        // The fields this app knows, with a value it does not take, beside what it does not know.
        assertEquals(bad, code(file("\${home}/a").replace("0644", "644")))
        assertEquals(bad, code(file("\${home}/a", src = "payload/none")))
        assertEquals(bad, code(file("\${app}/../a") + ""","later":1"""))
        assertEquals(bad, code(link("\${app}/l").replace("\"target\":\"a\"", "\"target\":\"\"") + ""","later":1"""))
        assertEquals(bad, code(""""files":{}""" + ""","later":1"""))
        assertEquals(bad, code(""""links":{}"""))
        assertEquals(bad, code(""""serialServices":[{"name":"R"}]"""))
        assertEquals(bad, manifest(good.replace(""""install":{""", """"install":[],"x":{""")))
    }
}

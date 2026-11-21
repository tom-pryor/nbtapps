// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.model.ManifestChannel
import com.nbtapps.store.model.ManifestSerialService
import com.nbtapps.store.model.PackageRequirements
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * Runs the reader over the packages the agent repo builds for its own tests: the same files the agent's reader
 * and the package tool are run over, with what each must say about them.
 */
class PackageCorpusTest {

    private val vectors = Json.parseToJsonElement(AgentVectors.file("packages/vectors.json").readText()).jsonObject

    private fun JsonObject.text(key: String) = getValue(key).jsonPrimitive.content

    @Test
    fun `every good package verifies, with the hashes, signature and flags the agent finds`() {
        val good = vectors.getValue("good").jsonArray.map { it.jsonObject }
        assertTrue("the corpus looks cut short", good.size >= 21)
        for (entry in good) {
            val what = entry.text("file")
            val key = AgentVectors.hex(entry.text("key"))
            val verified = PackageArchive.verify(AgentVectors.file("packages/$what"), listOf(AgentVectors.otherKey, key))

            assertEquals(what, entry.text("id"), verified.manifest.id)
            assertEquals(what, entry.text("version"), verified.manifest.version)
            assertEquals(what, entry.text("name"), verified.manifest.name)
            assertEquals(what, entry.text("sha256"), verified.sha256)
            assertEquals(what, entry.getValue("size").jsonPrimitive.long, verified.size)
            assertEquals(what, entry.text("manifestSha256"), verified.manifestSha256)
            assertEquals(what, entry.text("signature"), Hex.encode(verified.signature))
            assertArrayEquals(what, key, verified.key)
            assertEquals(what, entry.getValue("flags").jsonArray.map { it.jsonPrimitive.content }, verified.manifest.flags)
            assertFalse(what, verified.manifest.laterFormat)
        }
    }

    @Test
    fun `the serial services a package declares are read, in the order of the text`() {
        fun services(file: String) =
            PackageArchive.verify(AgentVectors.file("packages/good/$file"), listOf(AgentVectors.packageKey)).manifest.serialServices

        assertEquals(
            listOf(ManifestSerialService("TestRadio", "radiod"), ManifestSerialService("TestRadio-2", "radiod")),
            services("org.nbtapp.radio-1.0.0.nbtpkg"),
        )
        assertEquals(listOf(ManifestSerialService("TestRadio", "radio2d")), services("org.nbtapp.radio2-1.0.0.nbtpkg"))
        // A package that declares none has none.
        assertTrue(services("org.nbtapp.runtime-2.1.0.nbtpkg").isEmpty())
    }

    @Test
    fun `the channels a package names are read, in the order of the text, with the flag of a program it starts`() {
        fun manifest(file: String) =
            PackageArchive.verify(AgentVectors.file("packages/good/$file"), listOf(AgentVectors.packageKey)).manifest

        val chan = manifest("org.nbtapp.chan-1.0.0.nbtpkg")
        assertEquals(
            listOf(
                ManifestChannel("echo", "both", switched = false, program = "bin/chan_echo"),
                ManifestChannel("gated", "both", switched = true, program = "bin/chan_echo"),
                ManifestChannel("feed", "read", switched = false, program = "bin/chan_feed"),
            ),
            chan.channels,
        )
        assertEquals(listOf("starts-program"), chan.flags)
        val most = manifest("channels-most.nbtpkg")
        assertEquals(8, most.channels.size)
        assertEquals("c".repeat(32), most.channels[0].name)
        assertNull(most.channels[0].program)
        assertTrue(most.channels.single { it.name == "0-9" }.switched)
        assertFalse(most.laterFormat)
        // None at all.
        assertTrue(manifest("org.nbtapp.runtime-2.1.0.nbtpkg").channels.isEmpty())
    }

    @Test
    fun `the versions of the app a package names are read as a range, beside the agent's`() {
        fun requires(file: String) =
            PackageArchive.verify(AgentVectors.file("packages/$file"), listOf(AgentVectors.packageKey)).manifest.requires

        assertEquals(PackageRequirements(agent = ">=1.6.0", app = ">=0.8.0"), requires("good/requires-app.nbtpkg"))
        assertEquals(PackageRequirements(agent = ">=1.0.0"), requires("good/org.nbtapp.hello-1.2.0.nbtpkg"))
        // Not a range: refused, as the agent refuses it.
        try {
            requires("bad/requires-app-not-range.nbtpkg")
            fail("a requires.app that is no range was taken")
        } catch (e: PackageFormatException) {
            assertEquals(PackageFormatException.BAD_MANIFEST, e.code)
        }
    }

    @Test
    fun `the logs a package names are read, in the order of the text`() {
        fun logs(file: String) =
            PackageArchive.verify(AgentVectors.file("packages/good/$file"), listOf(AgentVectors.packageKey)).manifest.logs

        assertEquals(
            listOf(
                "\${data}/log/*.log", "\${app}/state.txt", "\${tmp}/crash-?.txt", "/var/log/hello.log",
                "/tmp/hello.log", "/dev/shmem/hello.log",
            ),
            logs("logs.nbtpkg"),
        )
        assertTrue(logs("org.nbtapp.hello-1.2.0.nbtpkg").isEmpty())
    }

    @Test
    fun `a log path is read as every case of the shared log path vectors says`() {
        val cases = AgentVectors.cases("log-paths.vectors")
        assertTrue(cases.size > 40)
        for (case in cases) {
            val path = case.input.decodeToString()
            when (case.verdict) {
                // A skipped path is the agent's to leave out: the app takes it, as the agent takes the manifest.
                "ok", "skip" -> assertTrue("line ${case.line}: $path", ManifestParser.isLogPath(path))
                "bad" -> assertFalse("line ${case.line}: $path", ManifestParser.isLogPath(path))
                else -> fail("line ${case.line} is not ok, skip or bad")
            }
        }
        // 255 characters, not bytes, as the agent counts them.
        assertTrue(ManifestParser.isLogPath("\${data}/" + "é".repeat(247)))
        assertFalse(ManifestParser.isLogPath("\${data}/" + "x".repeat(248)))
    }

    /**
     * Every broken package is refused with the code the agent gives, except one that `vectors.json` marks with `app`:
     * the planned differences between the readers (see [ManifestParser]), which the agent refuses and the app takes,
     * so that a later format, which an agent that knows it takes behind `requires.agent`, does not stop this app from
     * downloading the package. `no-settings`, whose only fault is in `settings`, is taken with no settings and no mark;
     * `no-channels`, whose only fault is in `channels`, with no channels and no mark. `later-format`, which has in
     * `install`, `requires` or `channels` what this app does not know, is taken with the mark, and with the risk flags
     * of what the app could read: hello's own, and `starts-program` for a channel with a program.
     */
    @Test
    fun `every broken package is refused, with the code the agent gives, but one the app takes as a later format`() {
        val bad = vectors.getValue("bad").jsonArray.map { it.jsonObject }
        assertTrue("the corpus looks cut short", bad.size >= 71)
        val (taken, others) = bad.partition { it["app"] != null }
        val (settings, later) = taken.partition { it.text("app") == "no-settings" }
        assertTrue("the corpus has no broken settings", settings.size >= 50)
        assertTrue(settings.all { it.text("file").startsWith("bad/settings-") })
        assertTrue("the corpus has too few of a later format", later.size >= 6)
        val hello = vectors.getValue("good").jsonArray.map { it.jsonObject }.single { it.text("file") == "good/org.nbtapp.hello-1.2.0.nbtpkg" }
        for (entry in others) {
            val what = "${entry.text("file")} (${entry.text("what")})"
            try {
                PackageArchive.verify(AgentVectors.file("packages/${entry.text("file")}"), listOf(AgentVectors.packageKey))
                fail("$what was accepted")
            } catch (e: PackageFormatException) {
                assertEquals(what, entry.text("code"), e.code)
            }
        }
        for (entry in taken) {
            val what = "${entry.text("file")} (${entry.text("what")})"
            assertTrue(what, entry.text("code") in listOf("bad-manifest", "unsupported-manifest"))
            val manifest = PackageArchive.verify(AgentVectors.file("packages/${entry.text("file")}"), listOf(AgentVectors.packageKey)).manifest
            assertEquals(what, "org.nbtapp.hello", manifest.id)
            when (entry.text("app")) {
                "no-settings" -> {
                    assertNull(what, manifest.settings)
                    assertFalse(what, manifest.laterFormat)
                }
                "no-channels" -> {
                    assertTrue(what, manifest.channels.isEmpty())
                    assertFalse(what, manifest.laterFormat)
                }
                "later-format" -> {
                    assertTrue(what, manifest.laterFormat)
                    // A channel the app could read, whose program the agent starts, adds its flag.
                    val starts = if (manifest.channels.any { it.program != null }) listOf("starts-program") else emptyList()
                    assertEquals(what, hello.getValue("flags").jsonArray.map { it.jsonPrimitive.content } + starts, manifest.flags)
                }
                else -> fail("$what: app is ${entry.text("app")}")
            }
        }
    }

    /**
     * What the package tool refuses to a package's author and the agent takes, the app takes too: a log in a folder
     * the agent does not know, and a `requires.app` with no `requires.agent`.
     */
    @Test
    fun `a log in a folder the agent does not know is taken, as the agent takes it`() {
        val skipped = vectors.getValue("skipped").jsonArray.map { it.jsonObject }
        assertTrue("the corpus looks cut short", skipped.size >= 4)
        for (entry in skipped) {
            val what = "${entry.text("file")} (${entry.text("what")})"
            val file = AgentVectors.file("packages/${entry.text("file")}")
            val manifest = PackageArchive.verify(file, listOf(AgentVectors.packageKey)).manifest
            assertFalse(what, manifest.laterFormat)
            when (entry.text("names")) {
                "requires.app" -> assertEquals(what, PackageRequirements(agent = null, app = ">=0.8.0"), manifest.requires)
                "channels" -> assertEquals(what, listOf("echo"), manifest.channels.map { it.name })
                "logs[1].path" -> {
                    assertEquals(what, 2, manifest.logs.size)
                    assertEquals(what, "\${data}/hello.log", manifest.logs[0])
                }
                else -> fail("$what names ${entry.text("names")}")
            }
        }
    }

    @Test
    fun `a detached signature is checked in place of the archive's own, with the outcome the agent gives`() {
        val detached = vectors.getValue("detached").jsonArray.map { it.jsonObject }
        assertTrue("the corpus looks cut short", detached.size >= 7)
        for (entry in detached) {
            val what = "${entry.text("file")} (${entry.text("what")})"
            val signature = AgentVectors.hex(entry.text("signature"))
            val file = AgentVectors.file("packages/${entry.text("file")}")
            // Only the package key is trusted: the author's own key, which signed some manifest.sig, is not.
            val outcome = try {
                val verified = PackageArchive.verify(file, listOf(AgentVectors.packageKey), signature)
                assertArrayEquals(what, signature, verified.signature)
                assertArrayEquals(what, AgentVectors.packageKey, verified.key)
                ""
            } catch (e: PackageFormatException) {
                e.code
            }
            assertEquals(what, entry.text("code"), outcome)
        }
    }
}

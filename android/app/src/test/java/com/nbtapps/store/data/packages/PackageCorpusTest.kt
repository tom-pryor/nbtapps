// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.model.ManifestSerialService
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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
    fun `the logs a package names are read, in the order of the text`() {
        fun logs(file: String) =
            PackageArchive.verify(AgentVectors.file("packages/good/$file"), listOf(AgentVectors.packageKey)).manifest.logs

        assertEquals(
            listOf("\${data}/log/*.log", "\${app}/state.txt", "\${tmp}/crash-?.txt", "/var/log/hello.log", "/tmp/hello.log"),
            logs("logs.nbtpkg"),
        )
        assertTrue(logs("org.nbtapp.hello-1.2.0.nbtpkg").isEmpty())
    }

    @Test
    fun `a log path is in the package's own folders, or under tmp or var and outside the agent's`() {
        for (path in listOf("\${data}/*", "\${tmp}/a", "/tmp/a/b", "/var/opt/nbtappx/y", "/tmp/nbtapps/a", "\${data}/.x")) {
            assertTrue(path, ManifestParser.isLogPath(path))
        }
        for (path in listOf(
            "", "\${data}", "\${data}/", "\${data}/x/", "\${data}/../x", "\${data}/*/x", "\${data}x/y", "\${plugins}/x",
            "\${home}/x", "x", "/etc/passwd", "/tmp", "/tmpx/y", "/tmp/nbtapp", "/tmp/nbtapp/x", "/var/opt/nbtapp",
            "/var/opt/nbtapp/phones.json", "\${data}/a\\b", "\${data}/a\nb",
        )) {
            assertFalse(path, ManifestParser.isLogPath(path))
        }
        // 255 characters, not bytes, as the agent counts them.
        assertTrue(ManifestParser.isLogPath("\${data}/" + "é".repeat(247)))
        assertFalse(ManifestParser.isLogPath("\${data}/" + "x".repeat(248)))
    }

    @Test
    fun `every broken package is refused, with the code the agent gives`() {
        val bad = vectors.getValue("bad").jsonArray.map { it.jsonObject }
        assertTrue("the corpus looks cut short", bad.size >= 71)
        for (entry in bad) {
            val what = "${entry.text("file")} (${entry.text("what")})"
            try {
                PackageArchive.verify(AgentVectors.file("packages/${entry.text("file")}"), listOf(AgentVectors.packageKey))
                fail("$what was accepted")
            } catch (e: PackageFormatException) {
                assertEquals(what, entry.text("code"), e.code)
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

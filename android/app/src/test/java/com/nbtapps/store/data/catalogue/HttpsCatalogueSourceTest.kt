// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import java.io.ByteArrayInputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.UnknownHostException
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class HttpsCatalogueSourceTest {

    /** What one address answers with. */
    private class Answer(
        val status: Int = 200,
        val body: ByteArray = ByteArray(0),
        val location: String? = null,
        val failure: IOException? = null,
    )

    /** A connection that goes nowhere: it answers with what the test has put at its address. */
    private class FakeConnection(url: URL, private val answer: Answer) : HttpURLConnection(url) {
        var open = true
        override fun connect() = Unit
        override fun disconnect() { open = false }
        override fun usingProxy() = false
        override fun getResponseCode(): Int = answer.failure?.let { throw it } ?: answer.status
        override fun getHeaderField(name: String?): String? = if (name == "Location") answer.location else null
        override fun getInputStream(): InputStream = ByteArrayInputStream(answer.body)
    }

    private val site = mutableMapOf<String, Answer>()
    private val connections = mutableListOf<FakeConnection>()

    /** Every address that was asked, in order. */
    private val asked get() = connections.map { it.url.toString() }

    private val source = HttpsCatalogueSource("https://catalogue.test/v1/", SourceKeys(ByteArray(32), ByteArray(32))) { url ->
        val answer = site[url.toString()] ?: throw UnknownHostException(url.host)
        FakeConnection(url, answer).also { connections += it }
    }

    private fun refused(block: suspend () -> Unit): String {
        val thrown = assertThrows(IOException::class.java) { runTest { block() } }
        return thrown.message.orEmpty()
    }

    @Test
    fun `a file is fetched from the source's folder, byte for byte`() = runTest {
        val index = byteArrayOf(0, 1, 2, -1, 10, 13)
        site["https://catalogue.test/v1/index.json"] = Answer(body = index)
        site["https://catalogue.test/v1/details/org.nbtapp.agent.json"] = Answer(body = "{}".encodeToByteArray())

        assertArrayEquals(index, source.fetch("index.json"))
        assertEquals("{}", source.fetch("details/org.nbtapp.agent.json").decodeToString())

        assertEquals(listOf("https://catalogue.test/v1/index.json", "https://catalogue.test/v1/details/org.nbtapp.agent.json"), asked)
        // Each connection is let go once its file is read, and none follows a redirect by itself.
        assertTrue(connections.none { it.open })
        assertTrue(connections.none { it.instanceFollowRedirects })
        assertTrue(connections.all { it.connectTimeout > 0 && it.readTimeout > 0 && !it.useCaches })
    }

    @Test
    fun `a path that is not plain names in folders is not asked for`() {
        for (path in listOf("", "/index.json", "details//a.json", "../index.json", "details/../../x", "./index.json",
            "index.json?x=1", "index.json#x", "a b.json", "details/%2e%2e/x", "https://other.test/index.json")) {
            assertEquals("Invalid catalogue path: $path", refused { source.fetch(path) })
        }
        assertTrue(asked.isEmpty())
    }

    @Test
    fun `a file the source does not have, and a server's error, are said in words`() {
        site["https://catalogue.test/v1/details/a.json"] = Answer(status = 404)
        site["https://catalogue.test/v1/index.json"] = Answer(status = 503, body = "busy".encodeToByteArray())

        assertEquals("catalogue.test doesn't have a.json", refused { source.fetch("details/a.json") })
        assertEquals("catalogue.test answered with an error (503)", refused { source.fetch("index.json") })
        assertTrue(connections.none { it.open })
    }

    @Test
    fun `a catalogue file that is too large is refused`() {
        site["https://catalogue.test/v1/index.json"] = Answer(body = ByteArray(4 * 1024 * 1024 + 1))
        site["https://catalogue.test/v1/icons/a.png"] = Answer(body = ByteArray(4 * 1024 * 1024))

        assertEquals("catalogue.test sent a catalogue file that is too large", refused { source.fetch("index.json") })
        runTest { assertEquals(4 * 1024 * 1024, source.fetch("icons/a.png").size) }
        assertTrue(connections.none { it.open })
    }

    @Test
    fun `a host that cannot be reached is said in words, whatever went wrong`() {
        site["https://catalogue.test/v1/index.json"] = Answer(failure = IOException("Connection reset"))

        // No such host: the connection is never made.
        val message = "Couldn't reach files.test. Check the phone's internet connection."
        assertEquals(message, refused { source.openArchive("https://files.test/a.nbtpkg") })
        // And one that ends while the answer is waited for.
        assertEquals(message.replace("files.test", "catalogue.test"), refused { source.fetch("index.json") })
        assertTrue(connections.none { it.open })
    }

    @Test
    fun `an archive is opened where its entry says, on whatever host that is`() = runTest {
        val archive = ByteArray(200_000) { (it % 251).toByte() }
        site["https://files.test/releases/a-1.0.0.nbtpkg"] = Answer(body = archive)

        val stream = source.openArchive("https://files.test/releases/a-1.0.0.nbtpkg")
        assertTrue(connections.single().open)   // the caller reads it, and closing it lets the connection go
        assertArrayEquals(archive, stream.use { it.readBytes() })
        assertFalse(connections.single().open)
    }

    @Test
    fun `redirects are followed from host to host, and the answer of each is let go`() = runTest {
        site["https://files.test/download/a.nbtpkg"] = Answer(status = 302, location = "https://store.test/blob/1?token=x%2Fy")
        site["https://store.test/blob/1?token=x%2Fy"] = Answer(status = 307, location = "/blob/2")
        site["https://store.test/blob/2"] = Answer(body = "archive".encodeToByteArray())

        assertEquals("archive", source.openArchive("https://files.test/download/a.nbtpkg").use { it.readBytes() }.decodeToString())
        assertEquals(listOf("https://files.test/download/a.nbtpkg", "https://store.test/blob/1?token=x%2Fy", "https://store.test/blob/2"), asked)
        assertTrue(connections.none { it.open })
    }

    @Test
    fun `nothing is asked of an address that is not https, at the start or after a redirect`() {
        site["http://files.test/a.nbtpkg"] = Answer(body = "plain".encodeToByteArray())
        site["https://files.test/a.nbtpkg"] = Answer(status = 301, location = "http://files.test/a.nbtpkg")

        assertEquals("Not an https address: http://files.test/a.nbtpkg", refused { source.openArchive("http://files.test/a.nbtpkg") })
        assertTrue(asked.isEmpty())
        assertEquals("Not an https address: http://files.test/a.nbtpkg", refused { source.openArchive("https://files.test/a.nbtpkg") })
        assertEquals(listOf("https://files.test/a.nbtpkg"), asked)
        for (address in listOf("file:///data/a.nbtpkg", "ftp://files.test/a.nbtpkg", "https:///a.nbtpkg")) {
            assertEquals("Not an https address: $address", refused { source.openArchive(address) })
        }
        assertEquals("Not a web address: a.nbtpkg", refused { source.openArchive("a.nbtpkg") })
        assertEquals(1, connections.size)
    }

    @Test
    fun `redirects that lead nowhere or never end are given up on`() {
        site["https://files.test/nowhere"] = Answer(status = 302)
        site["https://files.test/round"] = Answer(status = 302, location = "https://files.test/round")

        assertEquals("files.test sent a redirect that leads nowhere", refused { source.openArchive("https://files.test/nowhere") })
        connections.clear()
        assertEquals("files.test sent too many redirects", refused { source.openArchive("https://files.test/round") })
        assertEquals(6, connections.size)
        assertTrue(connections.none { it.open })
    }

    @Test
    fun `the source's own address has to be an https folder`() {
        val keys = SourceKeys(ByteArray(32), ByteArray(32))
        for (base in listOf("http://catalogue.test/v1/", "https://catalogue.test/v1", "catalogue.test/v1/")) {
            assertThrows(IllegalArgumentException::class.java) { HttpsCatalogueSource(base, keys) }
        }
    }
}

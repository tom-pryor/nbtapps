// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import java.io.ByteArrayOutputStream
import java.io.FilterInputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.MalformedURLException
import java.net.URL
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * A catalogue that is published over HTTPS: the files of its `v1/` folder at [base], and each archive at the
 * address its index entry names, which may be on another host.
 *
 * Nothing that comes out of here is trusted for where it came from. The index is checked against the source's
 * index key and every archive against its SHA-256 and the package key, by the callers. HTTPS is asked for all
 * the same, at every address and at every redirect, so that what the phone fetches is not readable or
 * replaceable on the way.
 *
 * It keeps no copy of what it fetches and sends nothing about the phone beyond the request itself.
 */
class HttpsCatalogueSource(
    /** Where the source's `v1/` folder is published: an https address that ends in `/`. */
    private val base: String,
    override val keys: SourceKeys,
    /** Opens the connection to an address. A test hands in one that leads to a server of its own. */
    private val connect: (URL) -> HttpURLConnection = { it.openConnection() as HttpURLConnection },
) : CatalogueSource {

    init {
        require(base.startsWith("https://") && base.endsWith("/")) { "Not an https address of a folder: $base" }
    }

    override suspend fun fetch(path: String): ByteArray = withContext(Dispatchers.IO) {
        // Paths other than the index come out of the index itself, so one is held to plain names in folders:
        // nothing that climbs out of the source's folder, and nothing that an address would read as a query.
        if (path.split('/').any { !SEGMENT.matches(it) || it == "." || it == ".." }) {
            throw IOException("Invalid catalogue path: $path")
        }
        open(base + path).use { input ->
            val bytes = ByteArrayOutputStream()
            val buffer = ByteArray(16 * 1024)
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                if (bytes.size() + n > MAX_FILE_BYTES) throw IOException("${host(base + path)} sent a catalogue file that is too large")
                bytes.write(buffer, 0, n)
            }
            bytes.toByteArray()
        }
    }

    override suspend fun openArchive(url: String): InputStream = withContext(Dispatchers.IO) { open(url) }

    /** The body of what [address] answers with, once it has answered 200. Redirects are followed, to https only. */
    private fun open(address: String): InputStream {
        var next = address
        repeat(MAX_REDIRECTS + 1) {
            val url = try {
                URL(next)
            } catch (e: MalformedURLException) {
                throw IOException("Not a web address: $next")
            }
            if (url.protocol != "https" || url.host.isEmpty()) throw IOException("Not an https address: $next")
            val connection = try {
                connect(url).apply {
                    connectTimeout = CONNECT_TIMEOUT_MS
                    readTimeout = READ_TIMEOUT_MS
                    instanceFollowRedirects = false   // followed here, so that each address is looked at
                    useCaches = false
                    setRequestProperty("User-Agent", USER_AGENT)
                }
            } catch (e: IOException) {
                throw unreachable(url, e)
            }
            val status = try {
                connection.responseCode
            } catch (e: IOException) {
                connection.disconnect()
                throw unreachable(url, e)
            }
            when (status) {
                HttpURLConnection.HTTP_OK -> return try {
                    Body(connection)
                } catch (e: IOException) {
                    connection.disconnect()
                    throw unreachable(url, e)
                }
                in REDIRECTS -> {
                    val location = connection.getHeaderField("Location")
                    connection.disconnect()
                    next = try {
                        URL(url, location ?: throw IOException("${url.host} sent a redirect that leads nowhere")).toString()
                    } catch (e: MalformedURLException) {
                        throw IOException("${url.host} sent a redirect that leads nowhere")
                    }
                }
                else -> {
                    connection.disconnect()
                    throw IOException(
                        if (status == HttpURLConnection.HTTP_NOT_FOUND) "${url.host} doesn't have ${url.path.substringAfterLast('/')}"
                        else "${url.host} answered with an error ($status)",
                    )
                }
            }
        }
        throw IOException("${host(address)} sent too many redirects")
    }

    private fun unreachable(url: URL, cause: IOException) =
        IOException("Couldn't reach ${url.host}. Check the phone's internet connection.", cause)

    private fun host(address: String) = try {
        URL(address).host
    } catch (e: MalformedURLException) {
        address
    }

    /** A response's body. Closing it lets the connection go too. */
    private class Body(private val connection: HttpURLConnection) : FilterInputStream(connection.inputStream) {
        override fun close() {
            try {
                super.close()
            } finally {
                connection.disconnect()
            }
        }
    }

    private companion object {
        /** The index, a package's details, an icon or a screenshot: the largest the catalogue's tool lets through is 2 MiB. */
        const val MAX_FILE_BYTES = 4 * 1024 * 1024
        const val MAX_REDIRECTS = 5
        const val CONNECT_TIMEOUT_MS = 15_000
        const val READ_TIMEOUT_MS = 30_000
        const val USER_AGENT = "NBTApps"
        val REDIRECTS = setOf(301, 302, 303, 307, 308)
        val SEGMENT = Regex("[A-Za-z0-9._-]+")
    }
}

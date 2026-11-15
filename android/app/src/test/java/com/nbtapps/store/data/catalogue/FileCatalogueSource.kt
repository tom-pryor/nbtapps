// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.AgentVectors
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream

/**
 * A catalogue source in folders, with the agent repo's test keys: its published files in [v1], such as the sample
 * catalogue's, and its archives in [archives], each found by the last part of the `url` that names it. A test makes
 * it unreachable with [unreachable].
 */
class FileCatalogueSource(private val v1: File?, private val archives: File) : CatalogueSource {
    override val keys = SourceKeys(index = AgentVectors.indexKey, packages = AgentVectors.packageKey)

    /** When set, everything fails with this message, as with no internet. */
    @Volatile
    var unreachable: String? = null

    override suspend fun fetch(path: String): ByteArray {
        unreachable?.let { throw IOException(it) }
        val file = v1?.let { File(it, path) }?.takeIf { it.isFile } ?: throw FileNotFoundException(path)
        return file.readBytes()
    }

    override suspend fun openArchive(url: String): InputStream {
        unreachable?.let { throw IOException(it) }
        val file = File(archives, url.substringAfterLast('/')).takeIf { it.isFile } ?: throw FileNotFoundException(url)
        return file.inputStream()
    }
}

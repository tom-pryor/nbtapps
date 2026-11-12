// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import java.io.IOException
import java.io.InputStream

/** A source's two public keys, both Ed25519: one signs `index.json`, the other each package's `manifest.sig`. */
class SourceKeys(val index: ByteArray, val packages: ByteArray)

/** Where one catalogue's published files come from. Paths are relative to the source's `v1/` folder. */
interface CatalogueSource {
    /** The keys everything from this source is checked against, or null: nothing from it is then accepted. */
    val keys: SourceKeys?

    /** Returns the exact bytes of the file at [path], or throws [IOException]. */
    suspend fun fetch(path: String): ByteArray

    /**
     * Opens the archive an index entry's `url` names. The caller reads it to the end, verifies what it read
     * and closes it; nothing that comes out of here is trusted.
     */
    suspend fun openArchive(url: String): InputStream

    companion object {
        const val INDEX_PATH = "index.json"

        /** The Ed25519 signature, made with the index key, over the exact bytes of `index.json`. */
        const val INDEX_SIGNATURE_PATH = "index.json.sig"
    }
}

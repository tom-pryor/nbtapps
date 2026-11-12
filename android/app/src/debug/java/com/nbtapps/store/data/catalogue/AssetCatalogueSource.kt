// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import android.content.Context
import com.nbtapps.store.data.crypto.Hex
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.IOException
import java.io.InputStream
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Serves the sample catalogue in the debug build's assets (`assets/catalogue/official`), laid out like a
 * source's published files, with the archives its index lists beside it. In the debug build it stands in for
 * the official catalogue, which the release build fetches over HTTPS ([HttpsCatalogueSource]).
 *
 * The files are a static fixture: this repo has no tool that makes them. They are built in the catalogue repo
 * (`tools/make_sample_catalogue.py` there) with the tool that builds the real catalogue, and copied in.
 *
 * The sample is signed with the agent repo's test keys, whose private halves are public. Anyone can sign with
 * them, so they vouch for nothing: this class and its keys are in the debug source set only, and must never
 * be part of a build that leaves the test bench.
 */
@Singleton
class AssetCatalogueSource @Inject constructor(
    @ApplicationContext private val context: Context,
) : CatalogueSource {

    override val keys = SourceKeys(
        index = requireNotNull(Hex.decodeOrNull(TEST_INDEX_KEY, 32)),
        packages = requireNotNull(Hex.decodeOrNull(TEST_PACKAGE_KEY, 32)),
    )

    override suspend fun fetch(path: String): ByteArray = withContext(Dispatchers.IO) {
        // Paths other than the index come out of the index itself, so don't let one climb out of the folder.
        if (path.split('/').any { it.isEmpty() || it == "." || it == ".." }) {
            throw IOException("Invalid catalogue path: $path")
        }
        context.assets.open("$ASSET_ROOT/v1/$path").use { it.readBytes() }
    }

    override suspend fun openArchive(url: String): InputStream = withContext(Dispatchers.IO) {
        // The sample's URLs lead nowhere; the file name at the end of one names the archive in the assets.
        val name = url.substringAfterLast('/')
        if (!url.startsWith(URL_PREFIX) || name.isEmpty() || !name.endsWith(".nbtpkg") || name.startsWith(".")) {
            throw IOException("The sample catalogue has no archive at $url")
        }
        context.assets.open("$ASSET_ROOT/packages/$name")
    }

    companion object {
        /** `tests/keys/test-index.pub` of the agent repo. A TEST key: see the class comment. */
        const val TEST_INDEX_KEY = "7f34d5afb32e82d4fe23fc92b3c5b01c75e7b637d49e0d4d8d9d8050f04a838d"

        /** `tests/keys/test-package.pub` of the agent repo. A TEST key: see the class comment. */
        const val TEST_PACKAGE_KEY = "18680457e3b23112c2f64935bdf852c263df25880d99ece34561c01c76a9758d"

        private const val ASSET_ROOT = "catalogue/official"
        private const val URL_PREFIX = "https://example.invalid/packages/"
    }
}

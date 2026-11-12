// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.data.crypto.Hex

/**
 * The official catalogue: where it is published, and the public halves of its two keys. They are the two files
 * in `keys/` of the catalogue repo, and the package key is the one built into the agent. Public keys vouch for
 * nothing by being known, so they may be in every build; which source a build uses is bound in its
 * `BuildTypeModule`.
 */
object OfficialCatalogue {
    /** The `v1/` folder that the catalogue repo's `tools/publish.py` publishes. */
    const val ADDRESS = "https://catalogue.nbtapps.com/v1/"

    /** `keys/index.pub` of the catalogue repo: fingerprint `f433 51c5 6418 466b`. */
    const val INDEX_KEY = "8479c681125f7689611afb2d1cc185ba462fe51d4c52562a25b1a11f491ce208"

    /** `keys/package.pub` of the catalogue repo: fingerprint `d041 6126 bb84 2c92`. */
    const val PACKAGE_KEY = "9ee0f6384ee29cdb6db26bd135d0391722134f849e9e42ab982cf93afb13051a"

    fun source(): CatalogueSource = HttpsCatalogueSource(
        base = ADDRESS,
        keys = SourceKeys(
            index = requireNotNull(Hex.decodeOrNull(INDEX_KEY, 32)),
            packages = requireNotNull(Hex.decodeOrNull(PACKAGE_KEY, 32)),
        ),
    )
}

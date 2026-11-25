// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.data.AppForeground
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * Keeps the catalogue from going stale while the process lives: each time the app comes to the front of the user, the
 * index is fetched again if it is due ([CatalogueRepository.refreshIfStale]). Nothing is fetched for an app that is
 * out of sight.
 */
class CatalogueKeeper(
    private val catalogue: CatalogueRepository,
    private val foreground: AppForeground,
    private val scope: CoroutineScope,
) {
    /** Begins to follow the app's comings and goings, for as long as [scope] lasts. Called once, as the app starts. */
    fun start() {
        scope.launch {
            foreground.inForeground.collect { shown ->
                // Beside the following, so that the app's next coming is seen though a fetch is still under way.
                if (shown) launch { catalogue.refreshIfStale() }
            }
        }
    }
}

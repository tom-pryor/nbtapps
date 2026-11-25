// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.data.FakeForeground
import com.nbtapps.store.model.PackageDetails
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test

/** When the catalogue is looked at for its age: as the app comes to the front, and at no other time. */
@OptIn(ExperimentalCoroutinesApi::class)
class CatalogueKeeperTest {

    /** Counts what is asked of it, and answers only once [gate] is completed, where one is set. */
    private class AskedCatalogue : CatalogueRepository {
        override val catalogue = MutableStateFlow(CatalogueState())
        var refreshes = 0
        var ifStale = 0
        var gate: CompletableDeferred<Unit>? = null

        override suspend fun refresh() {
            refreshes++
        }

        override suspend fun refreshIfStale() {
            ifStale++
            gate?.await()
        }

        override suspend fun details(packageId: String): Result<PackageDetails> = Result.failure(UnsupportedOperationException())
    }

    private val catalogue = AskedCatalogue()
    private val foreground = FakeForeground(shown = false)

    private fun TestScope.start() {
        CatalogueKeeper(catalogue, foreground, backgroundScope).start()
        runCurrent()
    }

    private fun TestScope.shown(shown: Boolean) {
        foreground.state.value = shown
        runCurrent()
    }

    @Test
    fun `each time the app comes to the front the catalogue is fetched if it is stale, and never as it goes`() = runTest {
        start()
        assertEquals("a process that started out of sight", 0, catalogue.ifStale)

        shown(true)
        assertEquals(1, catalogue.ifStale)
        shown(false)
        assertEquals(1, catalogue.ifStale)
        shown(true)
        assertEquals(2, catalogue.ifStale)
        assertEquals("whether it is due is the repository's to say", 0, catalogue.refreshes)
    }

    @Test
    fun `an app that is in front already as it starts to follow is taken as one that came`() = runTest {
        foreground.state.value = true

        start()

        assertEquals(1, catalogue.ifStale)
    }

    @Test
    fun `a fetch that is still under way does not hide the next coming`() = runTest {
        catalogue.gate = CompletableDeferred()
        start()
        shown(true)

        shown(false)
        shown(true)

        assertEquals(2, catalogue.ifStale)
    }
}

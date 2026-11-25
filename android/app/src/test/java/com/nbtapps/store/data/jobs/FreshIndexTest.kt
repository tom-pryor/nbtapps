// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.jobs

import com.nbtapps.store.data.catalogue.CatalogueRepository
import com.nbtapps.store.data.catalogue.CatalogueState
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.CatalogueSourceInfo
import com.nbtapps.store.model.PackageDetails
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** How long a choice waits for a newer index ([freshIndex]), in virtual time: each test says when the fetch ends. */
@OptIn(ExperimentalCoroutinesApi::class)
class FreshIndexTest {

    /** A catalogue whose one fetch ends when the test says so ([end]), whether or not anybody still waits for it. */
    private class FetchedCatalogue(held: CatalogueIndex?) : CatalogueRepository {
        override val catalogue = MutableStateFlow(CatalogueState(index = held))
        private val ended = CompletableDeferred<Unit>()
        var refreshes = 0
        var ifStale = 0

        override suspend fun refresh() {
            refreshes++
            ended.await()
        }

        override suspend fun refreshIfStale() {
            ifStale++
            ended.await()
        }

        fun end(index: CatalogueIndex?) {
            catalogue.value = CatalogueState(index = index ?: catalogue.value.index, error = "no network".takeIf { index == null })
            ended.complete(Unit)
        }

        override suspend fun details(packageId: String): Result<PackageDetails> = Result.failure(UnsupportedOperationException())
    }

    private val old = CatalogueIndex(1, CatalogueSourceInfo("test", "Test"), serial = 1)
    private val newer = old.copy(serial = 2)

    private fun TestScope.ask(catalogue: CatalogueRepository): Deferred<CatalogueIndex?> =
        async { freshIndex(catalogue) }.also { runCurrent() }

    @Test
    fun `a newer index that comes just in time is the one to choose from`() = runTest {
        val catalogue = FetchedCatalogue(old)
        val index = ask(catalogue)
        assertEquals(1, catalogue.ifStale)
        advanceTimeBy(4_999)
        runCurrent()
        assertTrue("it waits still", index.isActive)

        catalogue.end(newer)

        assertEquals(newer, index.await())
        assertEquals("an index was held", 0, catalogue.refreshes)
    }

    @Test
    fun `the wait ends after five seconds, and the one held is the one to choose from`() = runTest {
        val index = ask(FetchedCatalogue(old))
        advanceTimeBy(4_999)
        runCurrent()
        assertTrue("it waits still", index.isActive)

        advanceTimeBy(1)
        runCurrent()

        assertTrue(index.isCompleted)
        assertEquals(old, index.await())
    }

    @Test
    fun `a newer index that comes after the wait leaves the choice as it was`() = runTest {
        val catalogue = FetchedCatalogue(old)
        val index = ask(catalogue)
        advanceTimeBy(5_000)
        runCurrent()
        assertTrue(index.isCompleted)

        catalogue.end(newer)
        runCurrent()

        assertEquals(old, index.await())
        assertEquals("the next choice has it", newer, catalogue.catalogue.value.index)
    }

    @Test
    fun `where the fetch fails the one held is the one to choose from, at once`() = runTest {
        val catalogue = FetchedCatalogue(old)
        val index = ask(catalogue)

        catalogue.end(null)
        runCurrent()

        assertTrue(index.isCompleted)
        assertEquals(old, index.await())
    }

    @Test
    fun `with no index held the fetch is waited for as long as it takes`() = runTest {
        val catalogue = FetchedCatalogue(null)
        val index = ask(catalogue)
        assertEquals(1, catalogue.refreshes)
        advanceTimeBy(60_000)
        runCurrent()
        assertFalse(index.isCompleted)

        catalogue.end(newer)

        assertEquals(newer, index.await())
        assertEquals(0, catalogue.ifStale)
    }

    @Test
    fun `with no index to be had there is none to choose from`() = runTest {
        val catalogue = FetchedCatalogue(null)
        val index = ask(catalogue)

        catalogue.end(null)

        assertNull(index.await())
    }
}

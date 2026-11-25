// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.TestKeys
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class DefaultCatalogueRepositoryTest {

    private val keys = TestKeys()

    /**
     * Serves [files], with a signature of the index made as it is asked for, unless one is set. Counts how often the
     * index is asked for, and answers for it only once [gate] is completed, where one is set.
     */
    private inner class FakeSource : CatalogueSource {
        val files = mutableMapOf<String, String>()
        var failure: Throwable? = null
        var signature: ByteArray? = null
        var gate: CompletableDeferred<Unit>? = null
        var fetches = 0
        override var keys: SourceKeys? = SourceKeys(index = this@DefaultCatalogueRepositoryTest.keys.publicKey, packages = ByteArray(32))

        override suspend fun fetch(path: String): ByteArray {
            if (path == CatalogueSource.INDEX_PATH) {
                fetches++
                gate?.await()
            }
            failure?.let { throw it }
            if (path == CatalogueSource.INDEX_SIGNATURE_PATH) {
                return signature ?: this@DefaultCatalogueRepositoryTest.keys.sign(files.getValue(CatalogueSource.INDEX_PATH).encodeToByteArray())
            }
            return files[path]?.encodeToByteArray() ?: throw FileNotFoundException(path)
        }

        override suspend fun openArchive(url: String): InputStream = throw FileNotFoundException(url)
    }

    private fun index(serial: Long, name: String = "App") =
        """
        { "schema": 1, "source": { "id": "test", "name": "Test" }, "serial": $serial,
          "packages": [{ "id": "a", "name": "$name", "details": "details/a.json",
            "versions": [{ "version": "1.0.0", "url": "https://example.invalid/a.nbtpkg", "size": 1, "sha256": "00",
                           "manifestSha256": "11" }] }] }
        """

    private val source = FakeSource().apply { files[CatalogueSource.INDEX_PATH] = index(serial = 5) }

    /** What the repository's clock says, set by hand. */
    private var now = 1_000_000L

    /** The tests' own time and scope: the repository's fetches run in its background. */
    private val testScope = TestScope()
    private val repository = DefaultCatalogueRepository(source, testScope.backgroundScope, clock = { now })

    @Test
    fun `starts empty and holds the index after a refresh`() = testScope.runTest {
        assertNull(repository.catalogue.value.index)

        repository.refresh()

        val state = repository.catalogue.value
        assertEquals(listOf("a"), state.index?.packages?.map { it.id })
        assertFalse(state.refreshing)
        assertNull(state.error)
    }

    @Test
    fun `a failed first load reports the error`() = testScope.runTest {
        source.failure = IOException("no network")

        repository.refresh()

        val state = repository.catalogue.value
        assertNull(state.index)
        assertEquals("no network", state.error)
        assertFalse(state.refreshing)
    }

    @Test
    fun `a failed refresh keeps the index already held`() = testScope.runTest {
        repository.refresh()
        source.failure = IOException("no network")

        repository.refresh()

        val state = repository.catalogue.value
        assertNotNull(state.index)
        assertEquals("no network", state.error)
    }

    @Test
    fun `a later successful refresh clears the error and replaces the index`() = testScope.runTest {
        source.failure = IOException("no network")
        repository.refresh()
        source.failure = null
        source.files[CatalogueSource.INDEX_PATH] = index(serial = 6, name = "Renamed")

        repository.refresh()

        val state = repository.catalogue.value
        assertNull(state.error)
        assertEquals("Renamed", state.index?.packages?.single()?.name)
    }

    @Test
    fun `an index with a lower serial is rejected`() = testScope.runTest {
        repository.refresh()
        source.files[CatalogueSource.INDEX_PATH] = index(serial = 4, name = "Rolled back")

        repository.refresh()

        val state = repository.catalogue.value
        assertEquals(5L, state.index?.serial)
        assertEquals("App", state.index?.packages?.single()?.name)
        assertNotNull(state.error)
    }

    @Test
    fun `an index whose signature doesn't match is rejected, and the one held stays`() = testScope.runTest {
        repository.refresh()
        source.files[CatalogueSource.INDEX_PATH] = index(serial = 6, name = "Forged")
        source.signature = keys.sign(index(serial = 5).encodeToByteArray())

        repository.refresh()

        val state = repository.catalogue.value
        assertEquals("App", state.index?.packages?.single()?.name)
        assertNotNull(state.error)

        // A signature by another key is no better, nor is one of the wrong length.
        source.signature = TestKeys().sign(source.files.getValue(CatalogueSource.INDEX_PATH).encodeToByteArray())
        repository.refresh()
        assertEquals("App", repository.catalogue.value.index?.packages?.single()?.name)
        source.signature = ByteArray(10)
        repository.refresh()
        assertEquals("App", repository.catalogue.value.index?.packages?.single()?.name)
    }

    @Test
    fun `a source with no keys loads nothing`() = testScope.runTest {
        source.keys = null

        repository.refresh()

        assertNull(repository.catalogue.value.index)
        assertNotNull(repository.catalogue.value.error)
    }

    @Test
    fun `a malformed index reports an error`() = testScope.runTest {
        source.files[CatalogueSource.INDEX_PATH] = "{"

        repository.refresh()

        assertNull(repository.catalogue.value.index)
        assertNotNull(repository.catalogue.value.error)
    }

    @Test
    fun `details are fetched from the path the index gives`() = testScope.runTest {
        source.files["details/a.json"] = """{ "id": "a", "description": "About A" }"""
        repository.refresh()

        assertEquals("About A", repository.details("a").getOrThrow().description)
    }

    @Test
    fun `details fail for an unknown package, a missing file or the wrong package`() = testScope.runTest {
        repository.refresh()
        assertTrue(repository.details("unknown").isFailure)
        assertTrue(repository.details("a").isFailure)

        source.files["details/a.json"] = """{ "id": "b", "description": "About B" }"""
        assertTrue(repository.details("a").isFailure)
    }

    // ---- when a fetch that nobody asked for is due ----

    @Test
    fun `with nothing fetched yet, a fetch that nobody asked for is due`() = testScope.runTest {
        repository.refreshIfStale()

        assertEquals(1, source.fetches)
        assertEquals(5L, repository.catalogue.value.index?.serial)
    }

    @Test
    fun `an index is fetched again once it is 30 minutes old, and not before`() = testScope.runTest {
        repository.refreshIfStale()

        now += 30 * 60_000 - 1
        repository.refreshIfStale()
        assertEquals(1, source.fetches)

        now += 1
        source.files[CatalogueSource.INDEX_PATH] = index(serial = 6)
        repository.refreshIfStale()
        assertEquals(2, source.fetches)
        assertEquals(6L, repository.catalogue.value.index?.serial)

        // Its age is counted anew.
        now += 30 * 60_000 - 1
        repository.refreshIfStale()
        assertEquals(2, source.fetches)
    }

    @Test
    fun `an index fetched by a refresh is as fresh as one fetched by itself`() = testScope.runTest {
        repository.refresh()
        now += 30 * 60_000 - 1
        repository.refresh()
        assertEquals(2, source.fetches)

        now += 30 * 60_000 - 1
        repository.refreshIfStale()

        assertEquals(2, source.fetches)
    }

    @Test
    fun `the age is counted from the fetch's end`() = testScope.runTest {
        val gate = CompletableDeferred<Unit>().also { source.gate = it }
        val fetching = launch { repository.refreshIfStale() }
        runCurrent()
        now += 10 * 60_000
        gate.complete(Unit)
        fetching.join()

        now += 30 * 60_000 - 1
        repository.refreshIfStale()
        assertEquals(1, source.fetches)

        now += 1
        repository.refreshIfStale()
        assertEquals(2, source.fetches)
    }

    @Test
    fun `a fetch that failed is not tried again by itself within a minute, and a refresh does not wait for that`() = testScope.runTest {
        source.failure = IOException("no network")
        repository.refreshIfStale()
        now += 60_000 - 1
        repository.refreshIfStale()
        assertEquals(1, source.fetches)

        // A refresh does, and the minute begins again as it ends.
        repository.refresh()
        assertEquals(2, source.fetches)
        now += 60_000 - 1
        repository.refreshIfStale()
        assertEquals(2, source.fetches)

        now += 1
        repository.refreshIfStale()
        assertEquals(3, source.fetches)
    }

    @Test
    fun `an old index whose fetch failed is fetched again after a minute, not after another 30`() = testScope.runTest {
        repository.refresh()
        now += 30 * 60_000
        source.failure = IOException("no network")
        repository.refreshIfStale()
        assertEquals(2, source.fetches)
        source.failure = null

        now += 60_000
        repository.refreshIfStale()

        assertEquals(3, source.fetches)
        // It succeeded: the next is due in 30 minutes.
        now += 60_000
        repository.refreshIfStale()
        assertEquals(3, source.fetches)
    }

    // ---- one fetch at a time ----

    /** Starts [block], as far as it goes by itself. */
    private fun TestScope.begin(block: suspend () -> Unit): Job = launch(start = CoroutineStart.UNDISPATCHED) { block() }.also { runCurrent() }

    /** Holds the source's answers for the index back until the result is completed. */
    private fun hold(): CompletableDeferred<Unit> = CompletableDeferred<Unit>().also { source.gate = it }

    @Test
    fun `a refresh joins the one that is under way, and both have its outcome as they return`() = testScope.runTest {
        val gate = hold()
        val serials = ArrayList<Long?>()
        val one = begin { repository.refresh(); serials += repository.catalogue.value.index?.serial }
        val other = begin { repository.refresh(); serials += repository.catalogue.value.index?.serial }
        assertEquals(1, source.fetches)
        assertTrue(one.isActive && other.isActive)

        gate.complete(Unit)
        one.join()
        other.join()

        assertEquals(1, source.fetches)
        assertEquals(listOf<Long?>(5, 5), serials)
        assertFalse(repository.catalogue.value.refreshing)
        // Nothing is left of it: the next refresh fetches.
        repository.refresh()
        assertEquals(2, source.fetches)
    }

    @Test
    fun `a fetch that is due joins a refresh, within a minute of the last attempt too`() = testScope.runTest {
        source.failure = IOException("no network")
        repository.refresh()
        source.failure = null
        val gate = hold()
        val asked = begin { repository.refresh() }

        val joined = begin { repository.refreshIfStale() }
        assertTrue(joined.isActive)
        assertEquals(2, source.fetches)
        gate.complete(Unit)
        asked.join()
        joined.join()

        assertEquals(2, source.fetches)
        assertEquals(5L, repository.catalogue.value.index?.serial)
    }

    @Test
    fun `a caller that is cancelled does not stop the fetch, nor the others that wait for it`() = testScope.runTest {
        val gate = hold()
        val first = begin { repository.refresh() }
        val second = begin { repository.refresh() }

        first.cancelAndJoin()
        assertTrue(second.isActive)
        second.cancelAndJoin()
        assertTrue("the fetch goes on with nobody waiting", repository.catalogue.value.refreshing)
        gate.complete(Unit)
        runCurrent()

        assertEquals(1, source.fetches)
        assertEquals(5L, repository.catalogue.value.index?.serial)
        assertFalse(repository.catalogue.value.refreshing)
    }

    @Test
    fun `a caller that gives up waiting does not stop the fetch`() = testScope.runTest {
        val gate = hold()

        assertNull(withTimeoutOrNull(5_000) { repository.refreshIfStale() })
        assertNull(repository.catalogue.value.index)
        gate.complete(Unit)
        runCurrent()

        assertEquals(1, source.fetches)
        assertEquals(5L, repository.catalogue.value.index?.serial)
    }

    @Test
    fun `a refresh asked for as a fetch's outcome is shown is a fetch of its own`() = testScope.runTest {
        // Its outcome is seen, and the next asked for, within the first fetch's end.
        var second: Job? = null
        backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) {
            repository.catalogue.collect { state ->
                if (state.index?.serial == 5L && second == null) {
                    source.files[CatalogueSource.INDEX_PATH] = index(serial = 6)
                    second = launch(start = CoroutineStart.UNDISPATCHED) { repository.refresh() }
                }
            }
        }

        repository.refreshIfStale()
        requireNotNull(second).join()

        assertEquals(2, source.fetches)
        assertEquals(6L, repository.catalogue.value.index?.serial)
        assertFalse(repository.catalogue.value.refreshing)
    }

    @Test
    fun `a fetch that fails in a way nobody expected is a failure like any other`() = testScope.runTest {
        source.failure = IllegalStateException("not foreseen")

        repository.refresh()

        assertEquals("not foreseen", repository.catalogue.value.error)
        assertFalse(repository.catalogue.value.refreshing)
        source.failure = null
        repository.refresh()
        assertEquals(5L, repository.catalogue.value.index?.serial)
    }

    @Test
    fun `an error that ends the fetch's coroutine lets go of those who wait, and of the fetch`() = testScope.runTest {
        val errors = ArrayList<Throwable>()
        val scope = CoroutineScope(SupervisorJob() + StandardTestDispatcher(testScheduler) + CoroutineExceptionHandler { _, e -> errors += e })
        val repository = DefaultCatalogueRepository(source, scope, clock = { now })
        source.failure = AssertionError("out of everything")

        repository.refresh()

        assertEquals(listOf("out of everything"), errors.map { it.message })
        assertEquals("out of everything", repository.catalogue.value.error)
        assertFalse(repository.catalogue.value.refreshing)
        source.failure = null
        repository.refresh()
        assertEquals(5L, repository.catalogue.value.index?.serial)
    }

    @Test
    fun `a fetch whose scope ends as it runs, and one asked for after that, let go of those who wait`() = testScope.runTest {
        val scope = CoroutineScope(SupervisorJob() + StandardTestDispatcher(testScheduler))
        val repository = DefaultCatalogueRepository(source, scope, clock = { now })
        hold()
        val waiting = begin { repository.refresh() }

        scope.cancel()
        waiting.join()
        assertFalse(repository.catalogue.value.refreshing)
        assertEquals(1, source.fetches)

        repository.refresh()
        assertFalse(repository.catalogue.value.refreshing)
        assertEquals("nothing runs in it any more", 1, source.fetches)
    }

    // ---- what is shown of it ----

    /** Has an index held, [since] ago. */
    private suspend fun held(since: Long = 30 * 60_000L) {
        repository.refresh()
        now += since
    }

    @Test
    fun `a fetch that nobody asked for is not shown, and neither is its failure while an index is held`() = testScope.runTest {
        held()
        val gate = hold()
        val fetching = begin { repository.refreshIfStale() }
        assertEquals(2, source.fetches)
        assertFalse(repository.catalogue.value.refreshing)

        source.failure = IOException("no network")
        gate.complete(Unit)
        fetching.join()

        val state = repository.catalogue.value
        assertNull(state.error)
        assertFalse(state.refreshing)
        assertEquals(5L, state.index?.serial)
    }

    @Test
    fun `an error that was shown stays through a fetch that nobody asked for and that fails, and goes with one that succeeds`() = testScope.runTest {
        held()
        source.failure = IOException("no network")
        repository.refresh()
        assertEquals("no network", repository.catalogue.value.error)

        source.failure = IOException("something else")
        now += 60_000
        repository.refreshIfStale()
        assertEquals(3, source.fetches)
        assertEquals("no network", repository.catalogue.value.error)

        source.failure = null
        now += 60_000
        repository.refreshIfStale()
        assertEquals(4, source.fetches)
        assertNull(repository.catalogue.value.error)
    }

    @Test
    fun `with no index held the failure is shown though nobody asked for the fetch`() = testScope.runTest {
        val gate = hold()
        val fetching = begin { repository.refreshIfStale() }
        assertFalse(repository.catalogue.value.refreshing)

        source.failure = IOException("no network")
        gate.complete(Unit)
        fetching.join()

        assertEquals("no network", repository.catalogue.value.error)
        assertFalse(repository.catalogue.value.refreshing)
    }

    @Test
    fun `a refresh that joins a fetch nobody asked for shows it from then on, and its failure`() = testScope.runTest {
        held()
        val gate = hold()
        val fetching = begin { repository.refreshIfStale() }
        assertFalse(repository.catalogue.value.refreshing)

        val pulled = begin { repository.refresh() }
        assertTrue(repository.catalogue.value.refreshing)
        assertTrue(pulled.isActive)
        assertEquals(2, source.fetches)

        source.failure = IOException("no network")
        gate.complete(Unit)
        fetching.join()
        pulled.join()

        assertEquals("no network", repository.catalogue.value.error)
        assertFalse(repository.catalogue.value.refreshing)
        assertEquals(5L, repository.catalogue.value.index?.serial)
    }

    @Test
    fun `a fetch nobody asked for that joins a refresh leaves it shown, and its failure`() = testScope.runTest {
        held()
        val gate = hold()
        val pulled = begin { repository.refresh() }

        val joined = begin { repository.refreshIfStale() }
        assertTrue(repository.catalogue.value.refreshing)
        source.failure = IOException("no network")
        gate.complete(Unit)
        pulled.join()
        joined.join()

        assertEquals(2, source.fetches)
        assertEquals("no network", repository.catalogue.value.error)
        assertFalse(repository.catalogue.value.refreshing)
    }

    @Test
    fun `a refresh whose caller has gone is still one that was asked for`() = testScope.runTest {
        held()
        val gate = hold()
        val pulled = begin { repository.refresh() }
        pulled.cancelAndJoin()

        source.failure = IOException("no network")
        gate.complete(Unit)
        runCurrent()

        assertEquals("no network", repository.catalogue.value.error)
        assertFalse(repository.catalogue.value.refreshing)
    }
}

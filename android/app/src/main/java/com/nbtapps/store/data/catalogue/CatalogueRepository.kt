// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.data.crypto.Ed25519
import com.nbtapps.store.data.link.monotonicMillis
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.PackageDetails
import java.io.IOException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class CatalogueState(
    /** The last index that was accepted. Stays in place while refreshing and after a failed refresh. */
    val index: CatalogueIndex? = null,
    /** A fetch that [CatalogueRepository.refresh] asked for or joined is under way. One nobody asked for is not shown. */
    val refreshing: Boolean = false,
    /**
     * Why the last refresh failed, or null if it succeeded. With an index held, a fetch that nobody asked for with
     * [CatalogueRepository.refresh] leaves this as it was when it fails.
     */
    val error: String? = null,
)

interface CatalogueRepository {
    val catalogue: StateFlow<CatalogueState>

    /**
     * Fetches the index again, or joins the fetch that is under way. Failures land in [CatalogueState.error] rather
     * than being thrown. The state is the fetch's outcome by the time this returns.
     */
    suspend fun refresh()

    /**
     * Fetches the index again where none has been fetched yet or the last one was fetched long enough ago, unless the
     * last attempt has only just ended; joins a fetch that is under way. It is not shown as a refresh, nor is its
     * failure while an index is held. The state is the fetch's outcome by the time this returns, where there was one.
     */
    suspend fun refreshIfStale()

    suspend fun details(packageId: String): Result<PackageDetails>
}

/**
 * Holds the parsed index in memory, once its signature has been checked against the source's index key.
 * There is one built-in source for now; the on-disk copy and user-added sources from the plan are still to
 * come.
 *
 * One fetch of the index runs at a time, in [scope]: whoever asks meanwhile joins it, and it goes on when those who
 * wait for it are cancelled.
 */
class DefaultCatalogueRepository(
    private val source: CatalogueSource,
    /** Outlives every caller. The index is also checked and parsed in it, so it is not the main thread's. */
    private val scope: CoroutineScope,
    /**
     * A clock in milliseconds that only goes forward, and goes on while the phone sleeps: what the age of the index
     * held is measured on. `SystemClock.elapsedRealtime` in the app.
     */
    private val clock: () -> Long = ::monotonicMillis,
) : CatalogueRepository {

    private val state = MutableStateFlow(CatalogueState())

    override val catalogue: StateFlow<CatalogueState> = state.asStateFlow()

    /** A fetch of the index. */
    private class Fetch {
        /** [refresh] started or joined it. Guarded by [lock]. */
        var asked = false

        /** Completed once the outcome is in [state] and the fetch is no longer [current]. */
        val ended = CompletableDeferred<Unit>()
    }

    /**
     * Guards [current] and what goes with it, and is held for a fetch's changes of [state]: one that is joined as it
     * ends is joined before its outcome is taken, or not at all.
     */
    private val lock = Any()

    /** The fetch that is under way. Guarded by [lock]. */
    private var current: Fetch? = null

    /** When the last fetch that succeeded ended, on [clock]. Guarded by [lock]. */
    private var succeededAt: Long? = null

    /** When the last fetch ended, however it did, on [clock]. Guarded by [lock]. */
    private var endedAt: Long? = null

    override suspend fun refresh() = fetch(asked = true)

    override suspend fun refreshIfStale() = fetch(asked = false)

    /** Joins the fetch that is under way, or starts one if [asked] or if it is due; waits for it to end. */
    private suspend fun fetch(asked: Boolean) {
        var starting = false
        val fetch = synchronized(lock) {
            val fetch = current ?: if (asked || isDue()) Fetch() else return
            starting = fetch !== current
            current = fetch
            if (asked) {
                fetch.asked = true
                state.update { it.copy(refreshing = true) }
            }
            fetch
        }
        if (starting) start(fetch)
        fetch.ended.await()
    }

    /** Whether a fetch that nobody asked for is due. Call with [lock] held. */
    private fun isDue(): Boolean {
        val now = clock()
        val ended = endedAt
        if (ended != null && now - ended < RETRY_AFTER_MS) return false
        val succeeded = succeededAt ?: return true
        return now - succeeded >= STALE_AFTER_MS
    }

    private fun start(fetch: Fetch) {
        val job = scope.launch {
            val result = try {
                Result.success(loadIndex(lastSerial = state.value.index?.serial))
            } catch (e: Exception) {
                // Whatever it is: with nobody to throw it to, it is a failed fetch like any other.
                Result.failure(e)
            }
            end(fetch, result)
        }
        // Does nothing after the above. It is for a scope that has ended, and for an error that ends the process.
        job.invokeOnCompletion { end(fetch, Result.failure(it ?: IllegalStateException())) }
    }

    /** Takes [result] as the outcome of [fetch], unless it has ended already, and lets go of those who wait for it. */
    private fun end(fetch: Fetch, result: Result<CatalogueIndex>) {
        synchronized(lock) {
            if (current !== fetch) return
            current = null
            val now = clock()
            endedAt = now
            if (result.isSuccess) succeededAt = now
            state.update { held ->
                result.fold(
                    onSuccess = { CatalogueState(index = it) },
                    onFailure = {
                        val shown = fetch.asked || held.index == null
                        held.copy(refreshing = false, error = if (shown) it.describe() else held.error)
                    },
                )
            }
        }
        fetch.ended.complete(Unit)
    }

    override suspend fun details(packageId: String): Result<PackageDetails> = load {
        val path = state.value.index?.packages?.find { it.id == packageId }?.details
            ?: throw CatalogueFormatException("This package has no details")
        val details = CatalogueParser.parseDetails(source.fetch(path))
        if (details.id != packageId) throw CatalogueFormatException("The package details are for a different package")
        details
    }

    private suspend fun loadIndex(lastSerial: Long?): CatalogueIndex {
        val keys = source.keys ?: throw CatalogueFormatException("This version of the app has no catalogue source yet")
        val bytes = source.fetch(CatalogueSource.INDEX_PATH)
        // The signature covers the exact bytes, so it is checked before anything reads them.
        val signature = source.fetch(CatalogueSource.INDEX_SIGNATURE_PATH)
        if (!Ed25519.verify(keys.index, bytes, signature)) {
            throw CatalogueFormatException("The catalogue isn't signed by its source's key")
        }
        val index = CatalogueParser.parseIndex(bytes)
        if (lastSerial != null && index.serial < lastSerial) {
            throw CatalogueFormatException("The catalogue is older than the copy already loaded")
        }
        return index
    }

    private suspend fun <T> load(block: suspend () -> T): Result<T> = withContext(Dispatchers.Default) {
        try {
            Result.success(block())
        } catch (e: IOException) {
            Result.failure(e)
        } catch (e: CatalogueFormatException) {
            Result.failure(e)
        }
    }

    private fun Throwable.describe(): String = message ?: javaClass.simpleName

    companion object {
        /** How old the index held is when a fetch that nobody asked for is due. */
        const val STALE_AFTER_MS = 30 * 60_000L

        /** How long after a fetch has ended, however it did, the next that nobody asks for may start. */
        const val RETRY_AFTER_MS = 60_000L
    }
}

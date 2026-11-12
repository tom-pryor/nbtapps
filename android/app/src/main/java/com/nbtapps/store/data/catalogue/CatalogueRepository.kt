// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.catalogue

import com.nbtapps.store.data.crypto.Ed25519
import com.nbtapps.store.model.CatalogueIndex
import com.nbtapps.store.model.PackageDetails
import java.io.IOException
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

data class CatalogueState(
    /** The last index that was accepted. Stays in place while refreshing and after a failed refresh. */
    val index: CatalogueIndex? = null,
    val refreshing: Boolean = false,
    /** Why the last refresh failed, or null if it succeeded. */
    val error: String? = null,
)

interface CatalogueRepository {
    val catalogue: StateFlow<CatalogueState>

    /** Fetches the index again. Failures land in [CatalogueState.error] rather than being thrown. */
    suspend fun refresh()

    suspend fun details(packageId: String): Result<PackageDetails>
}

/**
 * Holds the parsed index in memory, once its signature has been checked against the source's index key.
 * There is one built-in source for now; the on-disk copy and user-added sources from the plan are still to
 * come.
 */
@Singleton
class DefaultCatalogueRepository @Inject constructor(
    private val source: CatalogueSource,
) : CatalogueRepository {

    private val state = MutableStateFlow(CatalogueState())
    private val refreshLock = Mutex()

    override val catalogue: StateFlow<CatalogueState> = state.asStateFlow()

    override suspend fun refresh() = refreshLock.withLock {
        state.update { it.copy(refreshing = true) }
        val result = load { loadIndex(lastSerial = state.value.index?.serial) }
        state.update { current ->
            result.fold(
                onSuccess = { CatalogueState(index = it) },
                onFailure = { current.copy(refreshing = false, error = it.describe()) },
            )
        }
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
}

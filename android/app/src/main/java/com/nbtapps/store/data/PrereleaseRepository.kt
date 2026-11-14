// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import androidx.datastore.core.CorruptionException
import androidx.datastore.core.DataStore
import androidx.datastore.core.DataStoreFactory
import androidx.datastore.core.Serializer
import androidx.datastore.core.handlers.ReplaceFileCorruptionHandler
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.update
import kotlinx.serialization.SerializationException
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.builtins.serializer
import kotlinx.serialization.json.Json

/**
 * The packages the user has switched on for pre-release versions, by id: each one's switch on its page. The
 * choice is the phone's, not a head unit's: it applies to every head unit, and stays until the user turns it off.
 * A package that is not here is offered only its stable versions.
 */
interface PrereleaseRepository {
    val switchedOn: Flow<Set<String>>

    /** Turns pre-release versions of the package [id] on or off. */
    suspend fun set(id: String, on: Boolean)
}

/** Keeps the switches in memory, for tests and previews. */
@Singleton
class InMemoryPrereleaseRepository @Inject constructor() : PrereleaseRepository {
    private val ids = MutableStateFlow<Set<String>>(emptySet())

    override val switchedOn: Flow<Set<String>> = ids

    override suspend fun set(id: String, on: Boolean) = ids.update { if (on) it + id else it - id }
}

/** Keeps the switches in a DataStore, so they are still there the next time the app starts. */
class DataStorePrereleaseRepository(private val store: DataStore<Set<String>>) : PrereleaseRepository {

    // Switches that cannot be read are all off: only stable versions are offered, which is never a surprise.
    override val switchedOn: Flow<Set<String>> =
        store.data.catch { e -> if (e is IOException) emit(emptySet()) else throw e }

    override suspend fun set(id: String, on: Boolean) {
        store.updateData { if (on) it + id else it - id }
    }
}

/** The ids as one JSON array, in order, which is how the DataStore file holds them. */
object PrereleaseSerializer : Serializer<Set<String>> {
    private val json = Json { ignoreUnknownKeys = true }
    private val serializer = ListSerializer(String.serializer())

    override val defaultValue: Set<String> = emptySet()

    override suspend fun readFrom(input: InputStream): Set<String> = try {
        json.decodeFromString(serializer, input.readBytes().decodeToString()).toSet()
    } catch (e: SerializationException) {
        throw CorruptionException("The pre-release switches file is not one this version can read", e)
    }

    override suspend fun writeTo(t: Set<String>, output: OutputStream) {
        output.write(json.encodeToString(serializer, t.sorted()).encodeToByteArray())
    }
}

/**
 * The DataStore that keeps the pre-release switches in [file]. Only one may be open on a file at a time; it is
 * closed when [scope] ends.
 */
fun prereleaseStore(file: File, scope: CoroutineScope): DataStore<Set<String>> = DataStoreFactory.create(
    serializer = PrereleaseSerializer,
    // A file this version cannot read leaves every switch off; there is nothing in it worth keeping aside.
    corruptionHandler = ReplaceFileCorruptionHandler { emptySet() },
    scope = scope,
    produceFile = { file },
)

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import androidx.datastore.core.CorruptionException
import androidx.datastore.core.DataStore
import androidx.datastore.core.DataStoreFactory
import androidx.datastore.core.Serializer
import androidx.datastore.core.handlers.ReplaceFileCorruptionHandler
import com.nbtapps.store.model.HeadUnit
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
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import kotlinx.serialization.SerializationException
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json

interface HeadUnitRepository {
    val headUnits: Flow<List<HeadUnit>>
    fun headUnit(id: String): Flow<HeadUnit?>
    suspend fun add(headUnit: HeadUnit)
    suspend fun remove(id: String)

    /** Replaces the head unit [id] with what [change] makes of it. Does nothing if it is not there. */
    suspend fun update(id: String, change: (HeadUnit) -> HeadUnit)
}

/** Keeps head units in memory, for tests and previews. */
@Singleton
class InMemoryHeadUnitRepository @Inject constructor() : HeadUnitRepository {
    private val units = MutableStateFlow<List<HeadUnit>>(emptyList())

    override val headUnits: Flow<List<HeadUnit>> = units

    override fun headUnit(id: String): Flow<HeadUnit?> = units.map { list -> list.find { it.id == id } }

    override suspend fun add(headUnit: HeadUnit) = units.update { it + headUnit }

    override suspend fun remove(id: String) = units.update { list -> list.filterNot { it.id == id } }

    override suspend fun update(id: String, change: (HeadUnit) -> HeadUnit) =
        units.update { list -> list.map { if (it.id == id) change(it) else it } }
}

/**
 * Keeps head units in a DataStore, so they are still there the next time the app starts: the list, the id
 * each agent gave, and the profile it last reported.
 */
class DataStoreHeadUnitRepository(private val store: DataStore<List<HeadUnit>>) : HeadUnitRepository {

    // A list that cannot be read is shown as empty; the next change then fails where the user can see it.
    override val headUnits: Flow<List<HeadUnit>> =
        store.data.catch { e -> if (e is IOException) emit(emptyList()) else throw e }

    override fun headUnit(id: String): Flow<HeadUnit?> = headUnits.map { list -> list.find { it.id == id } }

    override suspend fun add(headUnit: HeadUnit) {
        store.updateData { it + headUnit }
    }

    override suspend fun remove(id: String) {
        store.updateData { list -> list.filterNot { it.id == id } }
    }

    override suspend fun update(id: String, change: (HeadUnit) -> HeadUnit) {
        store.updateData { list -> list.map { if (it.id == id) change(it) else it } }
    }
}

/** The head units as one JSON array, which is how the DataStore file holds them. */
object HeadUnitsSerializer : Serializer<List<HeadUnit>> {
    private val json = Json { ignoreUnknownKeys = true }
    private val serializer = ListSerializer(HeadUnit.serializer())

    override val defaultValue: List<HeadUnit> = emptyList()

    override suspend fun readFrom(input: InputStream): List<HeadUnit> = try {
        json.decodeFromString(serializer, input.readBytes().decodeToString())
    } catch (e: SerializationException) {
        throw CorruptionException("The head units file is not one this version can read", e)
    }

    override suspend fun writeTo(t: List<HeadUnit>, output: OutputStream) {
        output.write(json.encodeToString(serializer, t).encodeToByteArray())
    }
}

/**
 * The DataStore that keeps the head units in [file]. Only one may be open on a file at a time; it is closed
 * when [scope] ends.
 */
fun headUnitStore(file: File, scope: CoroutineScope): DataStore<List<HeadUnit>> = DataStoreFactory.create(
    serializer = HeadUnitsSerializer,
    corruptionHandler = ReplaceFileCorruptionHandler {
        // A file this version cannot read is copied aside before the empty list is written over it.
        try {
            file.copyTo(File(file.path + ".unreadable"), overwrite = true)
        } catch (e: IOException) {
            // Nothing more can be done for it; the app must still start.
        }
        emptyList()
    },
    scope = scope,
    produceFile = { file },
)

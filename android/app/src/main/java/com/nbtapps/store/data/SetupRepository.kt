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
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.SerializationException
import kotlinx.serialization.json.Json

/**
 * Whether the user has skipped the set-up that walks a new user through adding the car, connecting, pairing and
 * having the agent start with the car. The choice is the phone's: it stays until it is changed back.
 */
interface SetupRepository {
    /** The user has skipped the set-up. False to begin with. */
    val skipped: Flow<Boolean>

    suspend fun setSkipped(skipped: Boolean)
}

/** Keeps the choice in memory, for tests and previews. */
@Singleton
class InMemorySetupRepository @Inject constructor() : SetupRepository {
    private val value = MutableStateFlow(false)

    override val skipped: Flow<Boolean> = value

    override suspend fun setSkipped(skipped: Boolean) {
        value.value = skipped
    }
}

/** Keeps the choice in a DataStore, so it is still there the next time the app starts. */
class DataStoreSetupRepository(private val store: DataStore<SetupSettings>) : SetupRepository {

    // A choice that cannot be read is taken as not made: the set-up is offered again, which loses nothing.
    override val skipped: Flow<Boolean> =
        store.data.catch { e -> if (e is IOException) emit(SetupSettings()) else throw e }.map { it.skipped }

    override suspend fun setSkipped(skipped: Boolean) {
        store.updateData { it.copy(skipped = skipped) }
    }
}

/** What the DataStore file of the set-up holds: one JSON object, so that more can go in it later. */
@Serializable
data class SetupSettings(val skipped: Boolean = false)

/** The settings as one JSON object, which is how the DataStore file holds them. */
object SetupSettingsSerializer : Serializer<SetupSettings> {
    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
    }

    override val defaultValue: SetupSettings = SetupSettings()

    override suspend fun readFrom(input: InputStream): SetupSettings = try {
        json.decodeFromString(SetupSettings.serializer(), input.readBytes().decodeToString())
    } catch (e: SerializationException) {
        throw CorruptionException("The set-up's file is not one this version can read", e)
    }

    override suspend fun writeTo(t: SetupSettings, output: OutputStream) {
        output.write(json.encodeToString(SetupSettings.serializer(), t).encodeToByteArray())
    }
}

/**
 * The DataStore that keeps the set-up's settings in [file]. Only one may be open on a file at a time; it is closed
 * when [scope] ends.
 */
fun setupStore(file: File, scope: CoroutineScope): DataStore<SetupSettings> = DataStoreFactory.create(
    serializer = SetupSettingsSerializer,
    // A file this version cannot read is taken as no choice made; there is nothing in it worth keeping aside.
    corruptionHandler = ReplaceFileCorruptionHandler { SetupSettings() },
    scope = scope,
    produceFile = { file },
)

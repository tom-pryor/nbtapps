// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.whatsnew

import androidx.datastore.core.CorruptionException
import androidx.datastore.core.DataStore
import androidx.datastore.core.DataStoreFactory
import androidx.datastore.core.Serializer
import androidx.datastore.core.handlers.ReplaceFileCorruptionHandler
import com.nbtapps.store.model.AppRelease
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import kotlinx.coroutines.CoroutineScope
import kotlinx.serialization.Serializable
import kotlinx.serialization.SerializationException
import kotlinx.serialization.json.Json

/** What is new in a version of the app, as the catalogue named it under `app`: its notes, a line each. */
@Serializable
data class VersionNotes(
    val versionCode: Long,
    val versionName: String,
    /** The release date as the catalogue gives it, or null where it gives none. */
    val released: String? = null,
    val lines: List<String> = emptyList(),
) {
    companion object {
        fun of(release: AppRelease) = VersionNotes(release.versionCode, release.versionName, release.released, release.notes)
    }
}

/**
 * What the DataStore file of what is new holds: one JSON object. [seen] is the version code whose notes have been
 * shown, or that needed none; [notes] are the kept notes, of two versions at most: those of the version that runs,
 * kept as a catalogue named them or by the copy before it as it handed the update to Android, and those of the
 * version the copy that runs last handed an update to, kept as it did so. The latter are still there where that
 * update did not happen, and do no harm: only the notes of the version that runs are ever shown.
 */
@Serializable
data class WhatsNewSettings(
    /** The layout of the file. One this version does not know is a later version's, and is not read. */
    val format: Int = FORMAT,
    val seen: Long? = null,
    val notes: List<VersionNotes> = emptyList(),
) {
    /**
     * These settings with [kept] among the notes, in place of any of the same version, and one more set at most: so
     * that the file holds the notes of the version that runs and of one update. Beside the notes of an update, those
     * of the version [running] stay; beside those of the version [running], the notes last kept of a later version,
     * for an update that may yet be done.
     */
    fun keeping(kept: VersionNotes, running: Long): WhatsNewSettings {
        val others = notes.filter { it.versionCode != kept.versionCode }
        val left = if (kept.versionCode == running) {
            others.filter { it.versionCode > running }.takeLast(1)
        } else {
            others.filter { it.versionCode == running }
        }
        return copy(notes = left + kept)
    }

    companion object {
        const val FORMAT = 1
    }
}

/** The settings as one JSON object, which is how the DataStore file holds them. */
object WhatsNewSerializer : Serializer<WhatsNewSettings> {
    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
    }

    override val defaultValue: WhatsNewSettings = WhatsNewSettings()

    override suspend fun readFrom(input: InputStream): WhatsNewSettings {
        val settings = try {
            json.decodeFromString(WhatsNewSettings.serializer(), input.readBytes().decodeToString())
        } catch (e: SerializationException) {
            throw CorruptionException("The file of what is new is not one this version can read", e)
        }
        if (settings.format > WhatsNewSettings.FORMAT) {
            throw CorruptionException("The file of what is new is of a later format (${settings.format})")
        }
        return settings
    }

    override suspend fun writeTo(t: WhatsNewSettings, output: OutputStream) {
        output.write(json.encodeToString(WhatsNewSettings.serializer(), t).encodeToByteArray())
    }
}

/**
 * The DataStore that keeps what is new in [file]. Only one may be open on a file at a time; it is closed when
 * [scope] ends.
 */
fun whatsNewStore(file: File, scope: CoroutineScope): DataStore<WhatsNewSettings> = DataStoreFactory.create(
    serializer = WhatsNewSerializer,
    // A file this version cannot read is taken as empty: at worst the notes of the version that runs are shown once
    // more, or not at all where the catalogue no longer has them.
    corruptionHandler = ReplaceFileCorruptionHandler { WhatsNewSettings() },
    scope = scope,
    produceFile = { file },
)

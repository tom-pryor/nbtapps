// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.packages

import com.nbtapps.store.data.catalogue.CatalogueSource
import com.nbtapps.store.model.PackageVersion
import java.io.IOException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** How far the download of one archive has got. An archive that is not in here is either stored or not asked for. */
sealed interface DownloadState {
    data class Running(val bytes: Long, val total: Long) : DownloadState
    data class Failed(val reason: String) : DownloadState
}

/**
 * Brings archives from the catalogue's source into the [PackageStore], ahead of the time they are needed in
 * the car. Download and install are separate steps, because the phone often has no internet while it is
 * joined to the car.
 */
class PackageDownloader(
    private val source: CatalogueSource,
    private val store: PackageStore,
    /** Downloads run here, so one goes on when the screen that started it is left. */
    private val scope: CoroutineScope,
) {
    private val states = MutableStateFlow<Map<String, DownloadState>>(emptyMap())

    /** The downloads that are running or have failed, by the archive's SHA-256. */
    val downloads: StateFlow<Map<String, DownloadState>> = states.asStateFlow()

    /** Starts downloading [version] of package [packageId], unless that is under way or done. */
    fun download(packageId: String, version: PackageVersion) {
        val key = version.sha256
        if (store.find(packageId, version.version)?.sha256 == key) return
        var claimed = false
        states.update { current ->
            claimed = current[key] !is DownloadState.Running
            if (claimed) current + (key to DownloadState.Running(0, version.size)) else current
        }
        if (!claimed) return
        scope.launch {
            val failure = try {
                val keys = source.keys ?: throw IOException("This version of the app has no catalogue source yet")
                source.openArchive(version.url).use { input ->
                    store.add(input, listOf(keys.packages), packageId to version) { bytes ->
                        states.update { it + (key to DownloadState.Running(bytes, version.size)) }
                    }
                }
                null
            } catch (e: PackageFormatException) {
                e.message
            } catch (e: IOException) {
                e.message ?: "The download failed"
            }
            states.update { if (failure == null) it - key else it + (key to DownloadState.Failed(failure)) }
        }
    }
}

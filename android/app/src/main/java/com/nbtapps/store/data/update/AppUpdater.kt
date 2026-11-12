// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.update

import com.nbtapps.store.data.catalogue.CatalogueSource
import com.nbtapps.store.data.crypto.Hex
import com.nbtapps.store.data.crypto.newSha256
import com.nbtapps.store.model.AppRelease
import java.io.File
import java.io.IOException
import java.io.InputStream
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Where the app's update of itself stands. */
sealed interface UpdateState {
    /** Nothing is under way. */
    data object Idle : UpdateState

    /**
     * Android does not yet let this app install apps. The user allows that for each app, once, in Android's own
     * settings; the update goes on by itself when the app is back in front and it is allowed.
     */
    data object NeedsPermission : UpdateState

    data class Downloading(val bytes: Long, val total: Long) : UpdateState

    /** The download was what the catalogue described, and Android has it. */
    data object Installing : UpdateState

    /** Android is asking the user, on a screen of its own. */
    data object Confirming : UpdateState

    /** [reason] is for the user. Another try starts from the beginning. */
    data class Failed(val reason: String) : UpdateState
}

/** Reads what an APK says of itself. Null for a file that is not one. */
fun interface ApkInspector {
    fun inspect(apk: File): ApkFacts?
}

/** What Android says of an install it was handed. */
sealed interface InstallEvent {
    /** It wants the user to agree, on its own screen. */
    data object NeedsUser : InstallEvent

    /** It did not install. [reason] is for the user. */
    data class Failed(val reason: String) : InstallEvent

    /**
     * It installed. An app that updates itself is ended by Android as the new copy takes its place, so this is
     * seldom seen by the copy that asked.
     */
    data object Done : InstallEvent
}

/** Hands an APK to Android as an update of this app. */
interface ApkInstaller {
    /** What Android says afterwards. It answers in its own time, and may ask the user first. */
    val events: SharedFlow<InstallEvent>

    /**
     * Whether Android lets this app install apps at all. It is off for every app until the user turns it on for
     * that app, in Android's settings ("Install unknown apps").
     */
    fun allowed(): Boolean

    /** Gives Android the whole of [apk]. Returns once it has it; throws [IOException] if it would not take it. */
    suspend fun install(apk: File)
}

/**
 * Updates the app from the release the catalogue names: downloads the APK, holds it to what the catalogue said
 * of it, and hands it to Android.
 *
 * Nothing the download says is believed before it is checked. The file has to have the size and the SHA-256 in
 * the signed index, and then to be this app, in the version named, signed with this copy's own key
 * ([apkProblem]). Android checks the signature again and takes nothing lower than what is installed.
 *
 * Android has to be allowed to take an app from this one, which the user grants once in Android's settings. That is
 * asked before anything is downloaded ([UpdateState.NeedsPermission]), and not left to Android's own question
 * at the end: Android gives up the install it was handed when the user leaves its question for the settings.
 *
 * An update ends the app: Android stops it as the new copy takes its place. So none is begun, and none is
 * handed to Android, while a job on a head unit needs the phone ([busy]); the job would lose its link in the
 * middle of a transfer.
 *
 * The download is kept in [dir], which holds nothing else and is emptied when the app starts: a file there is
 * either this run's or left over from an update that is done with.
 */
class AppUpdater(
    private val source: CatalogueSource,
    private val installed: InstalledApp,
    private val dir: File,
    private val inspector: ApkInspector,
    private val installer: ApkInstaller,
    /** Whether a job on a head unit needs the phone right now. */
    private val busy: () -> Boolean,
    /** The update runs here, so it goes on when the screen that started it is left. */
    private val scope: CoroutineScope,
) {
    private val current = MutableStateFlow<UpdateState>(UpdateState.Idle)

    /** The release an update was last asked for, which [permissionMayHaveChanged] goes on with. */
    @Volatile
    private var wanted: AppRelease? = null

    val state: StateFlow<UpdateState> = current.asStateFlow()

    /** What an earlier run left in [dir] is removed once, before anything of this run's is put there. */
    private val emptied = scope.launch(Dispatchers.IO) { dir.listFiles()?.forEach { it.delete() } }

    init {
        scope.launch {
            installer.events.collect { event ->
                current.update { now ->
                    // Android's word counts only for an install this run handed it.
                    if (now != UpdateState.Installing && now != UpdateState.Confirming) return@update now
                    when (event) {
                        InstallEvent.NeedsUser -> UpdateState.Confirming
                        is InstallEvent.Failed -> UpdateState.Failed(event.reason)
                        InstallEvent.Done -> UpdateState.Idle
                    }
                }
            }
        }
    }

    /**
     * The app is in front again, and the user may have been to Android's settings meanwhile. An update that
     * waited for them to allow it goes on if they did, and waits on if they did not.
     */
    fun permissionMayHaveChanged() {
        val release = wanted ?: return
        if (current.value == UpdateState.NeedsPermission && installer.allowed()) start(release)
    }

    /** Starts updating to [release], unless an update is under way. How it goes is in [state]. */
    fun start(release: AppRelease) {
        var claimed = false
        current.update { now ->
            claimed = now == UpdateState.Idle || now is UpdateState.Failed || now == UpdateState.NeedsPermission
            if (claimed) UpdateState.Downloading(0, release.size) else now
        }
        if (!claimed) return
        wanted = release
        scope.launch {
            val failure = try {
                run(release)
                null
            } catch (e: IOException) {
                e.message ?: "The update failed."
            }
            if (failure != null) current.value = UpdateState.Failed(failure)
        }
    }

    private suspend fun run(release: AppRelease) {
        if (busy()) throw IOException(BUSY)
        if (!installer.allowed()) {
            current.value = UpdateState.NeedsPermission
            return
        }
        val apk = withContext(Dispatchers.IO) {
            val file = download(release)
            apkProblem(inspector.inspect(file), release, installed)?.let { problem ->
                file.delete()
                throw IOException(problem)
            }
            file
        }
        // Asked again: the download took a while, and a job may have begun since.
        if (busy()) throw IOException(BUSY)
        current.value = UpdateState.Installing
        installer.install(apk)
    }

    /** The APK of [release] in [dir], whole and with the hash the catalogue gave. */
    private suspend fun download(release: AppRelease): File {
        emptied.join()
        if (!dir.isDirectory && !dir.mkdirs()) throw IOException("The phone has no place to keep the download.")
        val file = File(dir, "nbtapps-${release.versionCode}.apk")
        val part = File(dir, file.name + ".part")
        val digest = newSha256()
        var bytes = 0L
        try {
            source.openArchive(release.url).use { input: InputStream ->
                part.outputStream().use { out ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        bytes += n
                        // Stopped as soon as it is too long, so a wrong address cannot fill the phone.
                        if (bytes > release.size) throw IOException("The download is larger than the catalogue says.")
                        digest.update(buffer, 0, n)
                        out.write(buffer, 0, n)
                        current.value = UpdateState.Downloading(bytes, release.size)
                    }
                }
            }
            if (bytes != release.size) throw IOException("The download ended early. Check the phone's internet connection.")
            if (Hex.encode(digest.digest()) != release.sha256) throw IOException("The download isn't the file the catalogue names.")
            if (!part.renameTo(file)) throw IOException("The phone could not keep the download.")
        } finally {
            part.delete()
        }
        return file
    }

    private companion object {
        const val BUSY = "A job is running on a head unit. Update the app once it has finished."
    }
}

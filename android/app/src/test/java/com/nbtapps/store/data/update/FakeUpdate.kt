// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.update

import com.nbtapps.store.data.catalogue.CatalogueSource
import com.nbtapps.store.data.catalogue.SourceKeys
import com.nbtapps.store.data.crypto.sha256Hex
import com.nbtapps.store.model.AppRelease
import java.io.ByteArrayInputStream
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStream
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow

/** The SHA-256 of the certificate the release in these tests is signed with, and of another. */
const val RELEASE_CERT = "843dd6519def36860dfff183634424753b00a059846d6e2662ba608bff04d587"
const val OTHER_CERT = "b2fd811e2dd7133fe6e0a7931c3aaff0a4a7df5bb8950c6ee51184a78fd6f187"

const val APP_PACKAGE = "com.nbtapps.store"

/** The copy of the app that runs in these tests: 0.1.0, signed with the release key, on Android 14. */
val RUNNING = InstalledApp(APP_PACKAGE, versionCode = 100, versionName = "0.1.0", certSha256 = RELEASE_CERT, sdk = 34)

/** Stands for an APK's bytes. Nothing here opens one: what it says of itself is the inspector's to make up. */
fun apkBytes(version: String): ByteArray = "the app $version, as an APK".encodeToByteArray()

/** A release as the catalogue would name it for [body]. */
fun release(version: String = "0.2.0", code: Long = 200, body: ByteArray = apkBytes(version)) = AppRelease(
    versionCode = code,
    versionName = version,
    released = "2026-11-12",
    url = "https://catalogue.example/v1/app/nbtapps-$version.apk",
    size = body.size.toLong(),
    sha256 = sha256Hex(body),
    certSha256 = RELEASE_CERT,
    minSdk = 33,
    notes = listOf("Updates itself"),
)

/** A source whose only use is the APK at an address: [body], or with none an address that cannot be reached. */
class ApkSource(var body: ByteArray?) : CatalogueSource {
    override val keys: SourceKeys? = null
    val opened = mutableListOf<String>()

    /** Set to hold a download until it is completed. */
    var gate: CompletableDeferred<Unit>? = null

    override suspend fun fetch(path: String): ByteArray = throw FileNotFoundException(path)

    override suspend fun openArchive(url: String): InputStream {
        opened += url
        gate?.await()
        return ByteArrayInputStream(body ?: throw IOException("Couldn't reach catalogue.example. Check the phone's internet connection."))
    }
}

/** Android, as far as an install goes: it keeps what it was handed, and the test says what it answers. */
class FakeApkInstaller : ApkInstaller {
    /** No buffer: an answer is not sent until the updater has come for it, so it has dealt with the one before. */
    override val events = MutableSharedFlow<InstallEvent>()
    val handed = mutableListOf<ByteArray>()

    /** How many files it has been handed. The updater says it is installing a moment before the file is here. */
    val count = MutableStateFlow(0)
    var refusal: IOException? = null

    /** Whether the user has allowed the app to install apps. */
    var allowed = true

    override fun allowed() = allowed

    override suspend fun install(apk: File) {
        refusal?.let { throw it }
        handed += apk.readBytes()
        count.value = handed.size
    }
}

/** An updater with nothing to download and nothing in its way, for tests of what only holds one. */
fun idleUpdater(dir: File, scope: CoroutineScope) = AppUpdater(
    source = ApkSource(null),
    installed = RUNNING,
    dir = dir,
    inspector = { null },
    installer = FakeApkInstaller(),
    busy = { false },
    scope = scope,
)

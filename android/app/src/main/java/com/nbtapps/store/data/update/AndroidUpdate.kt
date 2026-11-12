// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.update

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.os.Build
import com.nbtapps.store.data.crypto.sha256Hex
import dagger.hilt.android.AndroidEntryPoint
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import java.io.IOException
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.withContext

/** SHA-256 of the one certificate a package is signed with, or null when there is not exactly one to name. */
private fun PackageInfo.certSha256(): String? {
    val signing = signingInfo ?: return null
    if (signing.hasMultipleSigners()) return null
    return signing.apkContentsSigners?.singleOrNull()?.let { sha256Hex(it.toByteArray()) }
}

/** The copy of the app that is running, as Android has it. */
fun installedApp(context: Context): InstalledApp {
    val info = context.packageManager.getPackageInfo(
        context.packageName,
        PackageManager.PackageInfoFlags.of(PackageManager.GET_SIGNING_CERTIFICATES.toLong()),
    )
    return InstalledApp(
        packageName = context.packageName,
        versionCode = info.longVersionCode,
        versionName = info.versionName.orEmpty(),
        certSha256 = info.certSha256(),
        sdk = Build.VERSION.SDK_INT,
    )
}

/** Reads an APK's package name, version code and certificate with Android's own parser, without installing it. */
class PackageManagerApkInspector(private val context: Context) : ApkInspector {
    override fun inspect(apk: File): ApkFacts? {
        val info = context.packageManager.getPackageArchiveInfo(
            apk.path,
            PackageManager.PackageInfoFlags.of(PackageManager.GET_SIGNING_CERTIFICATES.toLong()),
        ) ?: return null
        return ApkFacts(info.packageName, info.longVersionCode, info.certSha256())
    }
}

/**
 * Why Android did not install the update, in the app's own words. [status] is one of [PackageInstaller]'s
 * `STATUS_FAILURE` codes; [message] is what Android adds to it, which names the exact cause and is kept, in
 * brackets, for whoever has to find it.
 */
fun installFailure(status: Int, message: String?): String {
    val words = when (status) {
        PackageInstaller.STATUS_FAILURE_ABORTED -> return "The update was cancelled."
        PackageInstaller.STATUS_FAILURE_BLOCKED -> "This phone doesn't allow the update."
        PackageInstaller.STATUS_FAILURE_CONFLICT -> "The update doesn't fit the app that is installed."
        PackageInstaller.STATUS_FAILURE_INCOMPATIBLE -> "The update doesn't run on this phone."
        PackageInstaller.STATUS_FAILURE_INVALID -> "Android refused the update's file."
        PackageInstaller.STATUS_FAILURE_STORAGE -> "The phone has no room for the update."
        PackageInstaller.STATUS_FAILURE_TIMEOUT -> "Android gave up on the update. Try again."
        else -> "Android didn't install the update."
    }
    return if (message.isNullOrBlank()) words else "$words ($message)"
}

/**
 * Installs through a [PackageInstaller] session, which is how an app that is not a store updates itself.
 *
 * Android answers through [InstallResultReceiver]. When it chooses to, it asks the user on a screen of its own:
 * the screen to open is put in [confirmation], and the activity opens it, since Android lets only an app that is
 * in front open one. An update of the app by itself may go through with no question
 * (`USER_ACTION_NOT_REQUIRED`): Android decides, and a question is handled the same either way. On a phone
 * with Google Play Protect, that may ask as well, about an app it has not seen; that is between it and the user.
 */
@Singleton
class SessionApkInstaller @Inject constructor(
    @ApplicationContext private val context: Context,
) : ApkInstaller {

    private val answers = MutableSharedFlow<InstallEvent>(extraBufferCapacity = 8)

    override val events: SharedFlow<InstallEvent> = answers.asSharedFlow()

    /** Android's screen that asks the user about the update, until the activity has opened it. */
    val confirmation = MutableStateFlow<Intent?>(null)

    override fun allowed(): Boolean = context.packageManager.canRequestPackageInstalls()

    override suspend fun install(apk: File) = withContext(Dispatchers.IO) {
        try {
            val installer = context.packageManager.packageInstaller
            val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
                setAppPackageName(context.packageName)
                setSize(apk.length())
                setInstallReason(PackageManager.INSTALL_REASON_USER)
                setPackageSource(PackageInstaller.PACKAGE_SOURCE_DOWNLOADED_FILE)
                setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
            }
            val id = installer.createSession(params)
            val session = installer.openSession(id)
            try {
                session.openWrite("nbtapps.apk", 0, apk.length()).use { out ->
                    apk.inputStream().use { it.copyTo(out) }
                    session.fsync(out)
                }
                // Android fills its answer into this intent, so it has to be mutable; it names this app's own
                // receiver and nothing else can be sent it.
                val answer = PendingIntent.getBroadcast(
                    context,
                    id,
                    Intent(context, InstallResultReceiver::class.java),
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE,
                )
                session.commit(answer.intentSender)
            } catch (e: Exception) {
                session.abandon()
                throw e
            } finally {
                session.close()
            }
        } catch (e: IOException) {
            throw e
        } catch (e: RuntimeException) {
            // A session that Android will not open or take: no room, too many sessions, a refused permission.
            throw IOException("Android didn't take the update. (${e.message ?: e.javaClass.simpleName})", e)
        }
    }

    /** Android's answer to a session this app committed. */
    fun onResult(intent: Intent) {
        when (val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                confirmation.value = intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                answers.tryEmit(InstallEvent.NeedsUser)
            }
            PackageInstaller.STATUS_SUCCESS -> answers.tryEmit(InstallEvent.Done)
            else -> answers.tryEmit(
                InstallEvent.Failed(installFailure(status, intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE))),
            )
        }
    }
}

/** Where Android sends its answer to an install. It is not exported: only the system, holding the pending intent, reaches it. */
@AndroidEntryPoint
class InstallResultReceiver : BroadcastReceiver() {

    @Inject
    lateinit var installer: SessionApkInstaller

    override fun onReceive(context: Context, intent: Intent) = installer.onResult(intent)
}

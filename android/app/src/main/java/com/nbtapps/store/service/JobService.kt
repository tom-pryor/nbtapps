// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.PowerManager
import com.nbtapps.store.MainActivity
import com.nbtapps.store.R
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.JobKeeper
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.terminal.TerminalRepository
import dagger.hilt.android.AndroidEntryPoint
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/**
 * Keeps the app alive while it works with a head unit with the user's attention elsewhere: so that a long transfer
 * survives the screen turning off and the app being left, and an open terminal stays open. The link and what runs over
 * it belong to the repositories; this service only holds the process in the foreground, shows what it is held for,
 * and goes away once there is nothing left to hold it for. What it runs for is a list of reasons, the main one first
 * ([ForegroundWork.reasons]):
 *
 *  - a job on a head unit that needs the phone ([JobRepository.job]). A job that updates the agent itself ends the link
 *    on purpose: the agent restarts, and the app connects again by itself to see how it went ([JobView.reconnecting]).
 *    The job's link is not lost then, and this stays.
 *  - a pull of logs from a head unit ([JobRepository.logPull]): a transfer too, the other way, and as slow on Bluetooth
 *    serial.
 *  - a terminal being opened or open ([TerminalRepository.state]).
 *
 * The main reason's text is the notification's, and each other one adds a line below it. While archives or logs are
 * transferred it also holds a wake lock: the agent pulls, and a phone that dozes stops answering. Once the install has
 * begun the head unit finishes by itself, and the lock is let go. No lock is held for a terminal.
 */
@AndroidEntryPoint
class JobService : Service() {

    @Inject
    lateinit var jobs: JobRepository

    @Inject
    lateinit var terminal: TerminalRepository

    @Inject
    lateinit var headUnits: HeadUnitRepository

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        // A channel that exists takes the new name and description, and keeps what the user set for it.
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Head unit activity", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Shown while the app is working with a head unit: installs, log downloads, an open terminal, SSH access"
            },
        )
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "nbtapp:transfer")
            .apply { setReferenceCounted(false) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // The promise made by startForegroundService is kept before anything else, whatever there is to show. The head
        // units' names come with the first look below.
        val now = ForegroundWork.notice(ForegroundWork.reasons(jobs.job.value, jobs.logPull.value, terminal.state.value, emptyList()))
        startForeground(NOTIFICATION_ID, notification(now), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        if (!watching) {
            watching = true
            scope.launch {
                combine(jobs.job, jobs.logPull, terminal.state, headUnits.headUnits) { view, pull, open, units ->
                    ForegroundWork.notice(ForegroundWork.reasons(view, pull, open, units))
                }.collect { notice ->
                    if (notice == null) {
                        stopForeground(STOP_FOREGROUND_REMOVE)
                        stopSelf()
                    } else {
                        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(notice))
                        holdAwake(notice.awake)
                    }
                }
            }
        }
        return START_NOT_STICKY
    }

    private var watching = false

    override fun onDestroy() {
        scope.cancel()
        holdAwake(false)
        super.onDestroy()
    }

    private fun holdAwake(hold: Boolean) {
        val lock = wakeLock ?: return
        if (hold && !lock.isHeld) lock.acquire(MAX_TRANSFER_MILLIS) else if (!hold && lock.isHeld) lock.release()
    }

    /** The notification for [notice]; with none yet, one that says only that the app works with the head unit. */
    private fun notification(notice: ForegroundNotice?): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val builder = Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_head_unit)
            .setContentTitle(notice?.title ?: "Working with the head unit")
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
        if (notice == null) return builder.build()
        notice.lines.firstOrNull()?.let { builder.setContentText(it) }
        // Every line shows once the notification is opened out.
        if (notice.lines.size > 1) builder.setStyle(Notification.BigTextStyle().bigText(notice.lines.joinToString("\n")))
        notice.progress?.let { bar ->
            val permille = bar.permille
            if (permille != null) builder.setProgress(1000, permille, false) else builder.setProgress(0, 0, true)
        }
        return builder.build()
    }

    companion object {
        /** The channel's id from when it was called "Installs": a channel keeps its id, and the user's setting with it. */
        private const val CHANNEL_ID = "jobs"
        private const val NOTIFICATION_ID = 1

        /** A transfer that takes longer than this has stalled; the lock is not held for ever. */
        private const val MAX_TRANSFER_MILLIS = 2 * 60 * 60 * 1000L
    }
}

/** Starts [JobService] when a job begins, a pull of logs, or a terminal is opened. */
class ServiceJobKeeper @Inject constructor(
    @ApplicationContext private val context: Context,
) : JobKeeper {
    override fun jobRunning() {
        try {
            context.startForegroundService(Intent(context, JobService::class.java))
        } catch (e: IllegalStateException) {
            // The app is in the background and may not start a foreground service: a job found on reconnecting
            // from there. It runs on regardless, only without the notification, for as long as the system lets it.
        }
    }
}

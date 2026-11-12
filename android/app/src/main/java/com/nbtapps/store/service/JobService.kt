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
import com.nbtapps.store.data.jobs.JobKeeper
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.model.JobState
import dagger.hilt.android.AndroidEntryPoint
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Keeps the app alive while a job on a head unit needs the phone, so a long transfer survives the screen
 * turning off and the app being left. The link and the job belong to [JobRepository]; this service only
 * holds the process in the foreground for it, shows where the job stands, and goes away when the job has
 * ended or its link is gone.
 *
 * While archives are being transferred it also holds a wake lock: the agent pulls, and a phone that dozes
 * stops answering. Once the install has begun the head unit finishes by itself, and the lock is let go.
 *
 * A job that updates the agent itself ends the link on purpose: the agent restarts, and the app connects again
 * by itself to see how it went ([JobView.reconnecting]). The job's link is not lost then, and this stays.
 */
@AndroidEntryPoint
class JobService : Service() {

    @Inject
    lateinit var jobs: JobRepository

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Installs", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Shown while packages are transferred to a head unit and installed"
            },
        )
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "nbtapp:transfer")
            .apply { setReferenceCounted(false) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // The promise made by startForegroundService is kept before anything else, whatever the job's state.
        startForeground(NOTIFICATION_ID, notification(jobs.job.value), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        if (!watching) {
            watching = true
            scope.launch {
                jobs.job.collect { view ->
                    if (view == null || !view.needsPhone) {
                        stopForeground(STOP_FOREGROUND_REMOVE)
                        stopSelf()
                    } else {
                        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(view))
                        // And while the app waits to connect again after the agent restarted: a phone that dozes
                        // sits out the wait for far longer than it is meant to last.
                        holdAwake(view.job.state == JobState.FETCHING || view.reconnecting)
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

    private fun notification(view: JobView?): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val builder = Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_head_unit)
            .setContentTitle(view?.let { AgentText.state(it.job.state) } ?: "Working with the head unit")
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
        val job = view?.job
        if (job != null) {
            val step = if (job.steps > 0) "Step ${job.step} of ${job.steps}: " else ""
            builder.setContentText(step + AgentText.phase(job.phase) + " · keep the ignition on")
            val total = job.total
            val bytes = job.bytes
            if (total != null && bytes != null && total > 0) {
                builder.setProgress(1000, (bytes * 1000 / total).toInt().coerceIn(0, 1000), false)
            } else {
                builder.setProgress(0, 0, true)
            }
        }
        return builder.build()
    }

    companion object {
        private const val CHANNEL_ID = "jobs"
        private const val NOTIFICATION_ID = 1

        /** A transfer that takes longer than this has stalled; the lock is not held for ever. */
        private const val MAX_TRANSFER_MILLIS = 2 * 60 * 60 * 1000L
    }
}

/** Starts [JobService] when a job begins. */
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

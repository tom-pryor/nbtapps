// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.service

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.os.IBinder
import android.os.PowerManager
import com.nbtapps.store.MainActivity
import com.nbtapps.store.R
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.jobs.JobKeeper
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.jobs.JobView
import com.nbtapps.store.data.ssh.SshPort
import com.nbtapps.store.data.terminal.TerminalRepository
import com.nbtapps.store.model.HeadUnit
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
 *  - the SSH port for other apps, being switched on or on ([SshPort.state]), which brings an action that turns it off
 *    without opening the app.
 *
 * The main reason's text is the notification's, and each other one adds a line below it. A tap on it brings the app up,
 * on the terminal while one is open or being opened ([ForegroundWork.terminalToOpen]), else on "SSH from other apps"
 * while the port is on ([ForegroundWork.sshAccessToOpen]). While archives or logs are
 * transferred it also holds a wake lock: the agent pulls, and a phone that dozes stops answering. Once the install has
 * begun the head unit finishes by itself, and the lock is let go. No lock is held for a terminal or the port.
 */
@AndroidEntryPoint
class JobService : Service() {

    @Inject
    lateinit var jobs: JobRepository

    @Inject
    lateinit var terminal: TerminalRepository

    @Inject
    lateinit var headUnits: HeadUnitRepository

    @Inject
    lateinit var port: SshPort

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
        // The notification's action: the port goes off, and the look below then finds what is left to run for.
        if (intent?.action == ACTION_PORT_OFF) port.switchOff()
        // The promise made by startForegroundService is kept before anything else, whatever there is to show. The head
        // units' names are the ones the look below saw last; on a first start they come with its first look.
        val shell = terminal.state.value
        val now = ForegroundWork.notice(ForegroundWork.reasons(jobs.job.value, jobs.logPull.value, shell, names, port.state.value))
        // Asked to post again ([showAgain]), which is no start for a foreground service: with nothing to show, nothing is.
        if (intent?.action == ACTION_SHOW && now == null) {
            stopSelf(startId)
            return START_NOT_STICKY
        }
        val tap = Tap(ForegroundWork.terminalToOpen(shell), ForegroundWork.sshAccessToOpen(shell, port.state.value))
        startForeground(NOTIFICATION_ID, notification(now, tap), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        unseen = !notificationsAllowed(this)
        if (!watching) {
            watching = true
            scope.launch {
                combine(jobs.job, jobs.logPull, terminal.state, headUnits.headUnits, port.state) { view, pull, open, units, ssh ->
                    names = units
                    ForegroundWork.notice(ForegroundWork.reasons(view, pull, open, units, ssh)) to
                        Tap(ForegroundWork.terminalToOpen(open), ForegroundWork.sshAccessToOpen(open, ssh))
                }.collect { (notice, toOpen) ->
                    if (notice == null) {
                        unseen = false
                        stopForeground(STOP_FOREGROUND_REMOVE)
                        stopSelf()
                    } else {
                        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(notice, toOpen))
                        unseen = !notificationsAllowed(this@JobService)
                        holdAwake(notice.awake)
                    }
                }
            }
        }
        return START_NOT_STICKY
    }

    private var watching = false

    /** The saved head units, as the look last saw them: the names a notification posted from [onStartCommand] uses. */
    @Volatile
    private var names: List<HeadUnit> = emptyList()

    override fun onDestroy() {
        unseen = false
        scope.cancel()
        holdAwake(false)
        super.onDestroy()
    }

    private fun holdAwake(hold: Boolean) {
        val lock = wakeLock ?: return
        if (hold && !lock.isHeld) lock.acquire(MAX_TRANSFER_MILLIS) else if (!hold && lock.isHeld) lock.release()
    }

    /** Where a tap on the notification goes: a head unit's terminal, or else its "SSH from other apps", or the app. */
    private data class Tap(val terminal: String?, val sshAccess: String?)

    /**
     * The notification for [notice]; with none yet, one that says only that the app works with the head unit. A tap
     * brings the app up, on the screen [tap] names, if any.
     */
    private fun notification(notice: ForegroundNotice?, tap: Tap): Notification {
        val intent = Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
        tap.terminal?.let { intent.putExtra(MainActivity.EXTRA_TERMINAL, it) }
        tap.sshAccess?.let { intent.putExtra(MainActivity.EXTRA_SSH_ACCESS, it) }
        val open = PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
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
        notice.portAction?.let { label ->
            // To this service, which the app alone may start: the port goes off without the app being opened.
            val off = PendingIntent.getService(
                this,
                1,
                Intent(this, JobService::class.java).setAction(ACTION_PORT_OFF),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )
            builder.addAction(Notification.Action.Builder(Icon.createWithResource(this, R.drawable.ic_close), label, off).build())
        }
        return builder.build()
    }

    companion object {
        /** The channel's id from when it was called "Installs": a channel keeps its id, and the user's setting with it. */
        private const val CHANNEL_ID = "jobs"
        private const val NOTIFICATION_ID = 1

        /** The notification's action that turns the SSH port off. */
        private const val ACTION_PORT_OFF = "com.nbtapps.store.action.PORT_OFF"

        /** Posts the notification again: see [showAgain]. */
        private const val ACTION_SHOW = "com.nbtapps.store.action.SHOW"

        /**
         * The notification was last posted while notifications were not allowed, and Android dropped it: as when a
         * terminal opens while Android's question for them is still up.
         */
        @Volatile
        private var unseen = false

        private fun notificationsAllowed(context: Context): Boolean =
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

        /**
         * Posts the notification again where Android dropped it and notifications are allowed now ([ForegroundWork.postAgain]):
         * called as the app comes back in front, which it does once Android's question has been answered and once the
         * user is back from the phone's settings. The app is in front then, so the service may be started for it.
         */
        fun showAgain(context: Context) {
            if (!ForegroundWork.postAgain(unseen, notificationsAllowed(context))) return
            try {
                context.startService(Intent(context, JobService::class.java).setAction(ACTION_SHOW))
            } catch (e: IllegalStateException) {
                // Not in front after all: the next change of what it runs for posts it.
            }
        }

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

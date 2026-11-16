// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * Whether the user has the app in front of them: one of its activities is started. What is held open only for the
 * user, such as a link to a head unit with nothing running over it, is let go while it is not.
 */
interface AppForeground {
    /** True while one of the app's activities is started, give or take the pause [ActivityForeground] allows. */
    val inForeground: StateFlow<Boolean>
}

/** An app that is always in front of the user: for code that runs with no activities, as a unit test does. */
object AlwaysInForeground : AppForeground {
    override val inForeground: StateFlow<Boolean> = MutableStateFlow(true).asStateFlow()
}

/**
 * [AppForeground] from the activities' own starts and stops, which the application reports ([started], [stopped]).
 *
 * Coming back is reported at once. Leaving is reported only once no activity has been started for [leaveAfterMs]:
 * a rotation stops the activity and starts it again, and moving from one of the app's activities to another may
 * stop the one before the next has started. Neither is the user leaving.
 */
class ActivityForeground(
    private val scope: CoroutineScope,
    private val leaveAfterMs: Long = LEAVE_AFTER_MS,
) : AppForeground {
    private val state = MutableStateFlow(false)

    override val inForeground: StateFlow<Boolean> = state.asStateFlow()

    private val lock = Any()

    /** The activities that are started. Guarded by [lock]. */
    private var started = 0

    /** Rises with each start, so a leave that was waiting when an activity started again says nothing. Guarded by [lock]. */
    private var generation = 0

    /** An activity of the app was started. */
    fun started() {
        synchronized(lock) {
            started++
            generation++
            state.value = true
        }
    }

    /** An activity of the app was stopped. The last one to stop has the app leave the foreground, after the pause. */
    fun stopped() {
        val mine = synchronized(lock) {
            if (started > 0) started--
            if (started > 0) return
            generation
        }
        scope.launch {
            delay(leaveAfterMs)
            synchronized(lock) {
                if (started == 0 && generation == mine) state.value = false
            }
        }
    }

    companion object {
        /** Longer than a rotation or a move between the app's own activities takes, and short against anything else. */
        const val LEAVE_AFTER_MS = 700L
    }
}

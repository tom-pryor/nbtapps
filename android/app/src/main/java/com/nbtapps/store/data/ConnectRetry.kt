// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import java.io.IOException
import kotlinx.coroutines.delay

/**
 * Runs [connect] until it succeeds, pausing after a failure, for as long as a new try can still start within
 * [windowMillis] of the first one. The first pause is [pauseMillis], and each after it twice the one before, up to
 * [maxPauseMillis]: a car that refuses at once is not asked dozens of times. With the two the same, as by default,
 * every pause is alike.
 *
 * A car takes one Bluetooth serial link from a phone at a time. While another app on the phone has
 * that link, or is just opening it, a connect fails at once; a few seconds later the same connect
 * goes through.
 *
 * @param now a clock in milliseconds; only differences are used.
 * @param connect gets the number of the try, from 1.
 * @throws ConnectFailedException after the last try, naming the number of tries and the last failure.
 */
suspend fun <T> connectWithRetry(
    windowMillis: Long,
    pauseMillis: Long,
    now: () -> Long,
    maxPauseMillis: Long = pauseMillis,
    connect: suspend (attempt: Int) -> T,
): T {
    val start = now()
    var attempt = 0
    var pause = pauseMillis
    while (true) {
        attempt++
        try {
            return connect(attempt)
        } catch (e: IOException) {
            if (now() - start + pause >= windowMillis) {
                throw ConnectFailedException(attempt, (now() - start + 500) / 1000, e)
            }
        }
        delay(pause)
        pause = minOf(pause * 2, maxOf(maxPauseMillis, pauseMillis))
    }
}

/** Every try of a [connectWithRetry] failed. [cause] is the failure of the last of its [attempts]. */
class ConnectFailedException(val attempts: Int, val seconds: Long, cause: IOException) :
    IOException("Couldn't connect (${tries(attempts, seconds)}): ${cause.message}", cause) {

    /** How hard it was tried, such as `29 tries over 20 s`. */
    val tries: String get() = tries(attempts, seconds)

    private companion object {
        fun tries(attempts: Int, seconds: Long) = if (attempts == 1) "1 try" else "$attempts tries over $seconds s"
    }
}

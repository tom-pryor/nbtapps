// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.withTimeout

/**
 * [withTimeout] for a whole test. One that is not done by then fails with every thread's stack, in its message and on
 * the test's error output, so that a hang seen only elsewhere, on CI say, shows in the kept report what was waiting on
 * what. Suspended coroutines are on no thread, so what they wait on shows only through the threads that would wake them.
 */
suspend fun <T> withTestTimeout(timeMillis: Long, block: suspend CoroutineScope.() -> T): T =
    try {
        withTimeout(timeMillis, block)
    } catch (e: TimeoutCancellationException) {
        val stacks = allStacks()
        System.err.println(stacks)
        throw AssertionError("the test did not end within $timeMillis ms; every thread's stack then:\n$stacks", e)
    }

/** Every live thread's name, state and stack, by name. */
private fun allStacks(): String =
    Thread.getAllStackTraces().entries.sortedBy { it.key.name }.joinToString("\n\n") { (thread, stack) ->
        "\"${thread.name}\" ${thread.state}" + stack.joinToString("") { "\n    at $it" }
    }

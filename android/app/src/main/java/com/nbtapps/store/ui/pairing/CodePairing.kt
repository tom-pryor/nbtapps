// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairing

import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.ConnectionState
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.data.link.ProtocolConstants
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** What of [typed] is a code so far: its digits, as the agent reads them, and no more of them than a code has. */
internal fun codeDigits(typed: String): String = typed.filter { it in '0'..'9' }.take(ProtocolConstants.PAIRING_CODE_DIGITS)

/** A code from the car's screen as it is typed, and where handing it to the agent stands: see [CodePairing]. */
data class CodeEntry(
    /** What has been typed of the code: digits only, six at most. */
    val code: String = "",
    /** The code is with the agent, and its answer is awaited. */
    val working: Boolean = false,
    /** Why the last code did not pair the phone. */
    val error: String? = null,
) {
    /** The code is whole and none is on its way, so Pair can be tapped. */
    val complete: Boolean get() = code.length == ProtocolConstants.PAIRING_CODE_DIGITS && !working
}

/**
 * Pairing this phone by the code the car's screen shows, for a screen's view model: what is typed ([setCode]),
 * handing it to the agent ([pair]), and what came of it, in [state]. The code goes over the link the app already has
 * to the head unit, which waits for just that ([ConnectionState.NeedsPairing]); opening that link is the caller's.
 * [PairingCodeForm] shows it.
 *
 * The hand-over runs on [scope], which a view model gives as the app's: once the agent has issued the phone a key,
 * the key has to be kept and proved whether or not the user is still looking. Once the phone is paired, the head unit
 * is set up to start the agent with the car where [pair] was asked to ([AutostartSetup.begin]), and a set-up from an
 * earlier pairing is put away where it was not ([AutostartSetup.dismiss]).
 */
class CodePairing(
    private val jobs: JobRepository,
    private val setup: AutostartSetup,
    private val scope: CoroutineScope,
) {
    private val current = MutableStateFlow(CodeEntry())

    /** The code as typed, and how the last hand-over went. */
    val state: StateFlow<CodeEntry> = current.asStateFlow()

    /** Takes what the user typed: its digits ([codeDigits]). The last error goes with it. */
    fun setCode(typed: String) {
        current.update { it.copy(code = codeDigits(typed), error = null) }
    }

    /**
     * Hands the code to the agent, for the head unit [headUnitId], this app's id of the one the link is to. With
     * [autostart] the head unit is then set up to start the agent with the car: the caller says so where the box was
     * offered and ticked as Pair was tapped. Does nothing before the code is whole, or while one is on its way.
     *
     * Once the agent has answered, the code is cleared, for the next one if this one was wrong, and [CodeEntry.error]
     * says why it did not pair the phone.
     */
    fun pair(headUnitId: String, autostart: Boolean) {
        val before = current.value
        if (!before.complete) return
        // Two taps at once hand it over once.
        if (!current.compareAndSet(before, before.copy(working = true, error = null))) return
        scope.launch {
            val result = jobs.pair(before.code)
            // A set-up from an earlier pairing says nothing of this one.
            if (result.isSuccess) if (autostart) setup.begin(headUnitId) else setup.dismiss(headUnitId)
            // A code that was wrong is not typed over: it is cleared for the next one.
            current.value = CodeEntry(error = result.exceptionOrNull()?.let { it.message ?: "The phone couldn't be paired" })
        }
    }
}

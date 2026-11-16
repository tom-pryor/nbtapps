// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.ui.headunits.ChecklistMark
import com.nbtapps.store.ui.headunits.autostartSetupText

/** The first line of the prompt where head units are saved. */
internal const val PROMPT_LEAD = "Another app sent a pairing code."

/** The first line of the prompt where none is: for someone who has never seen the app. */
internal const val PROMPT_LEAD_NEW =
    "Another app sent a pairing code. NBTApps installs apps on your car's head unit, and pairing lets it reach yours."

/** The first line of the prompt: [PROMPT_LEAD_NEW] with no head unit saved ([anySaved] false), else [PROMPT_LEAD]. */
internal fun promptLead(anySaved: Boolean): String = if (anySaved) PROMPT_LEAD else PROMPT_LEAD_NEW

/** Under a device picked that is not saved yet. */
internal const val ADDED_ON_PAIR = "It is added to the app when you pair."

/** Before [ADDED_ON_PAIR], where the picker picked the device as the car the phone is connected to. */
internal const val PICKED_FOR_YOU = "Picked for you: it is the car this phone is connected to."

/** The prompt's main button: "Pair with BMW 12345", or "Pair" with no car chosen. */
internal fun pairLabel(name: String?): String = name?.let { "Pair with $it" } ?: "Pair"

/** What a failed page tells the user to do about the code. */
internal const val NEW_CODE = "Get a new code from the app that sent this one."

/**
 * The code's card: [code] grouped ([grouped]); [countdown], how long it is still good for ([countdown]), where the
 * other app said and the code is still of use; [ranOut], the code has run out, and is struck through.
 */
internal data class CodeCard(val code: String, val countdown: String?, val ranOut: Boolean)

/**
 * The code's card for [state], or null on a page that has none: once the phone is paired the code is spent. It
 * has run out on [PairRequestStep.TimeUp], or wherever no time is left. A code the agent refused has no countdown:
 * it is of no use however long it would have been good for.
 */
internal fun codeCard(state: PairRequestUiState): CodeCard? {
    val code = state.code ?: return null
    val step = state.step
    if (step is PairRequestStep.Paired || step is PairRequestStep.AlreadyPaired) return null
    val ranOut = step == PairRequestStep.TimeUp || state.remainingMs == 0L
    val left = state.remainingMs?.takeIf { !ranOut && step !is PairRequestStep.Refused }
    return CodeCard(grouped(code), left?.let(::countdown), ranOut)
}

/** One row of the Connecting and Pairing page. */
internal data class PairingRow(val mark: ChecklistMark, val text: String, val note: String? = null)

/** The note under the link's row while it is opened. */
internal const val LINK_NOTE = "If the car has only just started, this can take half a minute. The code is sent as soon as the link is up."

/**
 * The rows of the Connecting and Pairing page, at [step], for the car [name]: where this request added it
 * ([newCar]), that it was added; the link to the agent, being opened, then done once the code goes; and sending
 * the code, to come, then under way.
 */
internal fun pairingRows(step: PairRequestStep, newCar: Boolean, name: String): List<PairingRow> {
    val sending = step == PairRequestStep.Pairing
    return listOfNotNull(
        PairingRow(ChecklistMark.DONE, "$name added to the app").takeIf { newCar },
        if (sending) {
            PairingRow(ChecklistMark.DONE, "Opening a link to the agent…")
        } else {
            PairingRow(ChecklistMark.WORKING, "Opening a link to the agent…", LINK_NOTE)
        },
        PairingRow(if (sending) ChecklistMark.WORKING else ChecklistMark.TO_COME, "Sending the code"),
    )
}

/** What the box under the rows says while the code goes. */
internal const val STAY_HERE = "Stay on this screen until the phone is paired. Leaving now gives the attempt up."

/** What the paired page says of the app that sent the code. */
internal const val PAIRED_TEXT = "The car took the code. You can go back to the app that sent it at any time."

/** Under the set-up's card, where it offers to restart iDrive. */
internal const val RESTART_LOSES_LINKS =
    "Every app on this phone loses its link to the car when iDrive restarts, the one that sent the code too."

/** Under the set-up's card. */
internal const val GOES_ON = "This goes on if you leave. The car's page in NBTApps shows the same card."

/** Whether the set-up [setup] offers to restart iDrive, with [restartNeeded] as for [autostartSetupText]. */
internal fun offersRestart(setup: AutostartSetupState?, restartNeeded: Boolean): Boolean =
    setup != null && autostartSetupText(setup.step, restartNeeded).restart != null

/**
 * A page where the code did not pair the phone: [title], then [message] (in the error colour where [error]), and
 * the button: Retry where [retry], else Close.
 */
internal data class FailedPage(val title: String, val message: String, val error: Boolean, val retry: Boolean)

/** The failed page for [state], with the car's name [name] if one was chosen; null on a page that is no failure. */
internal fun failedPage(state: PairRequestUiState, name: String?): FailedPage? = when (val step = state.step) {
    PairRequestStep.TimeUp -> FailedPage("This code ran out", ranOutText(name), error = false, retry = false)
    is PairRequestStep.Refused -> FailedPage("The code didn't pair this phone.", step.message, error = true, retry = false)
    is PairRequestStep.NoLink -> FailedPage(
        "The code couldn't go to ${name ?: "the car"}: the app has no link to it.",
        step.reason,
        error = true,
        retry = state.canRetry,
    )
    else -> null
}

/** What the run-out page says, with the car's name [name], or null where no car was chosen. */
internal fun ranOutText(name: String?): String {
    val rest = "$NEW_CODE A code can't be typed or changed here."
    return if (name != null) "Nothing was sent to $name. $rest" else rest
}

/**
 * Whether a failed page says the car stays in the app, picked already when the next code comes: where it is saved
 * and is the only car saved, so the next request chooses it too.
 */
internal fun carStays(state: PairRequestUiState): Boolean = !state.picking && state.chosen != null && state.headUnits.size == 1

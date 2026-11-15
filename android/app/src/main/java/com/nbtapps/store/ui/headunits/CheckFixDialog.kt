// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.nbtapps.store.data.jobs.AgentRefusal
import com.nbtapps.store.data.jobs.AutostartSetup
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.data.jobs.JobRepository
import com.nbtapps.store.model.Check
import com.nbtapps.store.model.CheckFixResult
import com.nbtapps.store.model.CheckMove
import com.nbtapps.store.model.CheckState
import com.nbtapps.store.model.RestartKind
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/** Where the confirmation of one check's fix stands: see [CheckFixFlow]. */
sealed interface CheckFixStep {
    /** The check [id] is being run again, for the list as it is now and its token. [title] is the problem's. */
    data class Reading(val id: String, val title: String) : CheckFixStep

    /**
     * The list is shown, for the user to say yes to. [changed]: the files changed since the list the user was shown
     * before, which the agent refused the fix for.
     */
    data class Confirm(val check: Check, val changed: Boolean = false) : CheckFixStep

    /** The user said yes, and the fix runs. */
    data class Fixing(val check: Check) : CheckFixStep

    /** The fix ran: [result] says what it did, and what the check finds now. */
    data class Fixed(val check: Check, val result: CheckFixResult) : CheckFixStep

    /** The check finds nothing now: there is nothing left to fix. */
    data class Clear(val id: String, val title: String) : CheckFixStep

    /** It could not go on, for [reason]. */
    data class Failed(val id: String, val title: String, val reason: String) : CheckFixStep
}

/**
 * The confirmation of a check's fix, for one screen. A fix is never asked for without it: [open] runs the check again
 * first, so what is shown is the list as it is on the head unit now, with the token for that list; the user's yes
 * ([confirm]) sends that token. A list that changed meanwhile is refused by the agent as `stale`, and is read and
 * shown again, with a line that says so.
 *
 * Where a set-up asks for the fix ([open] with `handOver`), the yes is the set-up's to act on: the dialog closes,
 * and the token goes to it. A check that finds nothing now is handed over with no token: there is nothing to fix.
 *
 * The fix itself runs on [scope], which a screen gives as the app's: it goes on, and what it finds is kept, when
 * the screen is left.
 */
class CheckFixFlow(private val jobs: JobRepository, private val scope: CoroutineScope) {
    private val current = MutableStateFlow<CheckFixStep?>(null)

    /** The dialog to show, or null for none. */
    val state: StateFlow<CheckFixStep?> = current.asStateFlow()

    private val lock = Any()
    private var work: Job? = null

    /** Rises with each [open] and [close], so what was begun before publishes nothing more. */
    private var generation = 0

    /** Where the yes goes instead of a fix run here: the check's id and the token, or null where it finds nothing now. */
    private var handOver: ((String, String?) -> Unit)? = null

    /**
     * Opens the confirmation of the check [id]'s fix, in place of any there was. [title] is the problem's, as it is
     * kept, to show until the check has been read. [changed]: as in [CheckFixStep.Confirm].
     */
    fun open(id: String, title: String, changed: Boolean = false, handOver: ((String, String?) -> Unit)? = null) {
        synchronized(lock) {
            work?.cancel()
            val mine = ++generation
            this.handOver = handOver
            current.value = CheckFixStep.Reading(id, title)
            work = scope.launch {
                val step = read(id, title, changed)
                if (publish(mine, step) && step is CheckFixStep.Clear) handOver?.invoke(id, null)
            }
        }
    }

    /**
     * The fix's button on the set-up's card: the confirmation of what [setup] asks about on the head unit
     * [headUnitId], whose yes goes to the set-up, which runs the fix. Does nothing unless it asks.
     */
    fun openForSetup(setup: AutostartSetup, headUnitId: String) {
        val asked = setup.state.value?.takeIf { it.headUnitId == headUnitId }?.step as? AutostartStep.AskRemoval ?: return
        open(asked.problem.id, asked.problem.title, asked.changed) { id, token -> setup.confirmRemoval(id, token) }
    }

    /** The user's yes to the list shown. Does nothing unless a list is shown. */
    fun confirm() {
        val given = synchronized(lock) {
            val shown = current.value as? CheckFixStep.Confirm ?: return
            val token = shown.check.fix?.token ?: return
            val give = handOver
            if (give != null) {
                close()
                return@synchronized { give(shown.check.id, token) }
            }
            val mine = ++generation
            current.value = CheckFixStep.Fixing(shown.check)
            work = scope.launch {
                val result = jobs.fixCheck(shown.check.id, token)
                val failure = result.exceptionOrNull()
                val next = when {
                    failure == null -> CheckFixStep.Fixed(shown.check, result.getOrThrow())
                    failure is AgentRefusal && failure.code == "stale" -> read(shown.check.id, shown.check.title, changed = true)
                    else -> CheckFixStep.Failed(shown.check.id, shown.check.title, failure.message ?: "The head unit didn't answer")
                }
                publish(mine, next)
            }
            null
        }
        // Outside the lock: the set-up takes its own.
        given?.invoke()
    }

    /** Reads the check that failed again, and shows its list. */
    fun retry() {
        val failed = current.value as? CheckFixStep.Failed ?: return
        open(failed.id, failed.title, handOver = handOver)
    }

    /** Closes the dialog. A fix that runs goes on, and what it finds is kept; reading the list is given up. */
    fun close() {
        synchronized(lock) {
            if (current.value !is CheckFixStep.Fixing) work?.cancel()
            generation++
            handOver = null
            current.value = null
        }
    }

    private suspend fun read(id: String, title: String, changed: Boolean): CheckFixStep {
        val check = jobs.runCheck(id).getOrElse { return CheckFixStep.Failed(id, title, it.message ?: "The head unit didn't answer") }
        return when {
            !check.problem -> CheckFixStep.Clear(id, title)
            check.fix == null -> CheckFixStep.Failed(id, check.title, check.detail)
            else -> CheckFixStep.Confirm(check, changed)
        }
    }

    /** Shows [step], unless what [mine] began was replaced or closed meanwhile; says whether it was shown. */
    private fun publish(mine: Int, step: CheckFixStep): Boolean = synchronized(lock) {
        if (mine == generation) current.value = step
        mine == generation
    }

}

/**
 * The line that says the checks found something on the head unit [name] that is not put aside, [count] things in
 * all: on the catalogue's banner and on the pairing screens, each with a way to the head unit's page.
 */
fun checksLine(count: Int, name: String): String =
    if (count == 1) "1 thing on $name needs a look." else "$count things on $name need a look."

/** [text], from [checksLine], with the way to the head unit's page: [onOpen]. */
@Composable
fun ChecksLine(text: String, onOpen: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(text, Modifier.weight(1f), color = MaterialTheme.colorScheme.tertiary)
        TextButton(onClick = onOpen) { Text("Show") }
    }
}

/** What the dialog says when the files changed since the last list. */
internal const val CHECK_FILES_CHANGED = "The files on the head unit changed since they were listed. This is the list as it is now."

/** What it says of the restart where the fix needs one: the set-up's card and the agent's card offer it. */
internal const val CHECK_FIX_RESTART = "iDrive has to restart afterwards. That restarts the whole head unit, and the app loses its link."

/** What the fix of [check] did, as [result] says, in a line or two. */
internal fun checkFixSummary(check: Check, result: CheckFixResult): String {
    val listed = check.count > 0 || check.moves.isNotEmpty()
    val moved = when (result.moved.size) {
        0 -> null
        1 -> "1 folder was moved aside."
        else -> "${result.moved.size} folders were moved aside."
    }
    val removed = when {
        check.count == 0 -> null
        result.removed == 1 -> "1 path was removed."
        else -> "${result.removed} paths were removed."
    }
    val done = listOfNotNull(moved, removed).joinToString(" ").ifEmpty { if (listed) "Nothing was changed." else "Done." }
    val still = if (result.state == CheckState.PROBLEM) " Something is still there: the head unit's page shows what." else ""
    return done + still
}

/** The programs a fix stopped first, in a line; null where it stopped none. */
internal fun checkFixStopped(result: CheckFixResult): String? =
    result.stopped.takeIf { it.isNotEmpty() }?.let { "Stopped first: ${it.joinToString(", ")}." }

/** The heading over the folders a fix moves aside, which it keeps. */
internal fun movesHeading(count: Int): String =
    if (count == 1) "This folder is moved aside, not removed, and kept where it goes:" else "These $count folders are moved aside, not removed, and kept where they go:"

/** The heading over the paths a fix removes. */
internal fun removesHeading(count: Int): String =
    if (count == 1) "This is removed from the head unit:" else "These $count are removed from the head unit:"

/**
 * The confirmation of a check's fix at [step]: the fix's detail, every path it removes, and that iDrive has to
 * restart afterwards where it does. The confirm button is the fix's own label. While the fix runs it shows that,
 * and afterwards what was removed, what was left and which programs were stopped.
 */
@Composable
fun CheckFixDialog(step: CheckFixStep, onConfirm: () -> Unit, onRetry: () -> Unit, onClose: () -> Unit) {
    val fixing = step is CheckFixStep.Fixing
    AlertDialog(
        // While the fix runs, the dialog stays: it says how it went once it is done.
        onDismissRequest = { if (!fixing) onClose() },
        title = {
            Text(
                when (step) {
                    is CheckFixStep.Confirm -> step.check.title
                    is CheckFixStep.Fixing -> step.check.title
                    is CheckFixStep.Fixed -> step.check.title
                    is CheckFixStep.Reading -> step.title
                    is CheckFixStep.Clear -> step.title
                    is CheckFixStep.Failed -> step.title
                },
            )
        },
        text = {
            Column(
                Modifier.heightIn(max = 420.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                when (step) {
                    is CheckFixStep.Reading -> Working("Reading what is on the head unit…")
                    is CheckFixStep.Confirm -> Confirmation(step)
                    is CheckFixStep.Fixing -> Working("Working…")
                    is CheckFixStep.Fixed -> Outcome(step)
                    is CheckFixStep.Clear -> Text("Nothing is in the way any more.")
                    is CheckFixStep.Failed -> Text(step.reason, color = MaterialTheme.colorScheme.error)
                }
            }
        },
        confirmButton = {
            when (step) {
                is CheckFixStep.Confirm -> TextButton(onClick = onConfirm) { Text(step.check.fix?.label ?: "Remove") }
                is CheckFixStep.Failed -> TextButton(onClick = onRetry) { Text("Try again") }
                is CheckFixStep.Reading, is CheckFixStep.Fixing -> Unit
                else -> TextButton(onClick = onClose) { Text("Close") }
            }
        },
        dismissButton = {
            when (step) {
                is CheckFixStep.Confirm, is CheckFixStep.Reading, is CheckFixStep.Failed -> TextButton(onClick = onClose) { Text("Cancel") }
                else -> Unit
            }
        },
    )
}

@Composable
private fun Confirmation(step: CheckFixStep.Confirm) {
    val check = step.check
    if (step.changed) Text(CHECK_FILES_CHANGED, color = MaterialTheme.colorScheme.error)
    check.fix?.detail?.takeIf { it.isNotEmpty() }?.let { Text(it) }
    if (check.moves.isNotEmpty()) {
        Text(movesHeading(check.moves.size), style = MaterialTheme.typography.titleSmall)
        Moves(check.moves)
    }
    if (check.found.isNotEmpty()) {
        Text(removesHeading(check.found.size), style = MaterialTheme.typography.titleSmall)
        Paths(check.found)
    }
    if (check.fix?.restart != null && check.fix.restart != RestartKind.NONE) {
        Text(CHECK_FIX_RESTART, color = MaterialTheme.colorScheme.tertiary)
    }
}

@Composable
private fun Outcome(step: CheckFixStep.Fixed) {
    val result = step.result
    Text(checkFixSummary(step.check, result))
    if (result.moved.isNotEmpty()) {
        Text("Moved aside:", style = MaterialTheme.typography.titleSmall)
        Moves(result.moved)
    }
    if (result.left.isNotEmpty()) {
        Text("Left on the head unit:", style = MaterialTheme.typography.titleSmall)
        Paths(result.left)
        if (result.leftMore > 0) Text("and ${result.leftMore} more.")
    }
    checkFixStopped(result)?.let { Text(it) }
    if (result.restart != RestartKind.NONE) {
        Text("iDrive has to restart to finish. Restart iDrive is on the head unit's page.", color = MaterialTheme.colorScheme.tertiary)
    }
}

/** Folders that are moved aside: each where it is, and under it where it goes. */
@Composable
internal fun Moves(moves: List<CheckMove>) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        moves.forEach { move ->
            Column {
                Text(move.from, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                Text("to ${move.to}", style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }
        }
    }
}

/** Paths, one to a line, as the agent gave them. */
@Composable
internal fun Paths(paths: List<String>) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        paths.forEach { Text(it, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace) }
    }
}

@Composable
private fun Working(text: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        Text(text)
    }
}

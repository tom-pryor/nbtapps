// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.nbtapps.store.data.jobs.AutostartStep

/** What the box on the pairing screens is called. */
internal const val ENABLE_AUTOSTART = "Enable autostart"

/** The line under it: what ticking it does. */
internal const val ENABLE_AUTOSTART_NOTE =
    "The agent then starts with the car. The iDrive plugin does the starting, and is installed if the car doesn't have it. " +
        "Last, if an app was installed on the head unit by hand, you are asked about it."

/**
 * The box on both pairing screens that has the head unit set up to start the agent with the car once the phone is
 * paired: see [AutostartSetup][com.nbtapps.store.data.jobs.AutostartSetup]. It is ticked to begin with.
 */
@Composable
fun EnableAutostartBox(checked: Boolean, onCheckedChange: (Boolean) -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    Row(
        modifier
            .fillMaxWidth()
            .toggleable(value = checked, enabled = enabled, role = Role.Checkbox, onValueChange = onCheckedChange)
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Checkbox(checked = checked, onCheckedChange = null, enabled = enabled)
        Column(Modifier.padding(start = 12.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(ENABLE_AUTOSTART)
            Text(ENABLE_AUTOSTART_NOTE, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** What the set-up's card says and offers at one step: see [autostartSetupText]. */
internal data class AutostartSetupText(
    /** Where the set-up stands, or why it stopped. */
    val status: String,
    /** It is under way: a spinner goes with [status]. */
    val working: Boolean = false,
    /** It stopped short: [status] is the reason, and Retry is offered. */
    val failed: Boolean = false,
    /** What is still to be done by hand: iDrive has to restart, and the button that asks for it is offered. */
    val restart: String? = null,
    /** Said before [status]: how far the set-up went, where it now asks about what was found. */
    val lead: String? = null,
    /** The set-up waits for the user's answer about what was found, which this says: the fix's button and Skip are offered. */
    val question: String? = null,
    /** With [question]: the fix's button, the agent's own label. */
    val fixLabel: String? = null,
    /** A line after [status] about a fix that left something or could not be done. */
    val note: String? = null,
)

/**
 * What the card of a set-up at [step] says. [restartNeeded]: the agent says over the live link that iDrive has to
 * restart, as it does once the plugin is installed; it is offered only once the set-up is done.
 */
internal fun autostartSetupText(step: AutostartStep, restartNeeded: Boolean): AutostartSetupText = when (step) {
    AutostartStep.Checking -> AutostartSetupText("Checking the head unit…", working = true)
    is AutostartStep.AskRemoval -> AutostartSetupText(
        step.problem.title,
        lead = startsText(step.switchedOn),
        question = removalQuestion(step.problem.moves.size, step.problem.paths.size, step.changed),
        fixLabel = step.problem.fix?.label ?: "Put it right",
    )
    is AutostartStep.Removing -> AutostartSetupText("Putting this right: ${step.problem.title}…", working = true, lead = startsText(step.switchedOn))
    AutostartStep.Downloading -> AutostartSetupText("Getting the iDrive plugin…", working = true)
    is AutostartStep.Installing -> AutostartSetupText("Installing the iDrive plugin…", working = true)
    AutostartStep.SwitchingOn -> AutostartSetupText("Switching on…", working = true)
    is AutostartStep.Done -> AutostartSetupText(
        startsText(step.changed) + if (step.moved > 0 || step.removed > 0) " Of what was installed by hand, ${doneCounts(step.moved, step.removed)}." else "",
        restart = (if (step.changed) AUTOSTART_RESTART_NEEDED else AUTOSTART_RESTART_TO_FINISH).takeIf { restartNeeded },
        note = when {
            step.fixFailed != null -> "${step.fixFailed} The head unit's page still has it."
            step.left -> AUTOSTART_SOMETHING_LEFT
            else -> null
        },
    )
    AutostartStep.Restarting -> AutostartSetupText(AUTOSTART_RESTARTING)
    is AutostartStep.Failed -> AutostartSetupText(step.reason, failed = true)
}

/** Whether the agent starts with the car now that the set-up [switchedOn] it, or did already. */
internal fun startsText(switchedOn: Boolean): String =
    if (switchedOn) "The agent now starts with the car." else "The agent already starts with the car."

/** "1 folder" or "2 paths": [count] of [thing]. */
private fun counted(count: Int, thing: String): String = if (count == 1) "1 $thing" else "$count ${thing}s"

/**
 * What the card asks about a problem whose fix moves [moves] folders aside and removes [paths] paths. [changed]:
 * the files changed since the list was last shown.
 */
internal fun removalQuestion(moves: Int, paths: Int, changed: Boolean): String {
    val move = if (moves > 0) "moves ${counted(moves, "folder")} aside, where ${if (moves == 1) "it is" else "they are"} kept" else null
    val remove = if (paths > 0) "removes ${counted(paths, "path")}" else null
    val does = listOfNotNull(move, remove).joinToString(", and ")
    val text = "Last, the checks found this on the head unit. Its fix $does. Each one is shown to you before anything " +
        "changes; Skip leaves it all as it is, and the head unit's page keeps it."
    return if (changed) "The files changed since they were listed. $text" else text
}

/** What the fixes did, as the end of a sentence: "1 folder was moved aside and 4 paths were removed". */
internal fun doneCounts(moved: Int, removed: Int): String {
    val move = if (moved > 0) "${counted(moved, "folder")} ${if (moved == 1) "was" else "were"} moved aside" else null
    val remove = if (removed > 0) "${counted(removed, "path")} ${if (removed == 1) "was" else "were"} removed" else null
    return listOfNotNull(move, remove).joinToString(" and ")
}

/** Where a fix left something on the head unit. */
internal const val AUTOSTART_SOMETHING_LEFT = "Something is still there: the head unit's page shows what."

/** Where only a fix needs the restart: the set-up switched nothing on. */
internal const val AUTOSTART_RESTART_TO_FINISH =
    "iDrive has to restart to finish. That restarts the whole head unit, and the app loses its link."

internal const val AUTOSTART_RESTART_NEEDED =
    "iDrive has to restart before it loads the plugin. That restarts the whole head unit, and the app loses its link."

internal const val AUTOSTART_RESTARTING =
    "iDrive is restarting. The agent starts about 15 seconds after it is back. Connect again then."

/**
 * The set-up at [step], on a card: where it stands, the fix's button and Skip where it asks about what was installed
 * by hand, Retry where it stopped short, and "Restart iDrive now" once it is done where iDrive has to restart
 * ([restartNeeded], as for [autostartSetupText]). [onRemove] opens the confirmation that lists what the fix moves
 * aside and removes ([CheckFixDialog]).
 * [onOpenJob], where the caller offers it, leads to the job screen while the plugin installs; [onDismiss], where
 * offered, hides a set-up that has ended.
 */
@Composable
fun AutostartSetupCard(
    step: AutostartStep,
    restartNeeded: Boolean,
    onRetry: () -> Unit,
    onRestart: () -> Unit,
    onRemove: () -> Unit,
    onSkip: () -> Unit,
    modifier: Modifier = Modifier,
    onOpenJob: (() -> Unit)? = null,
    onDismiss: (() -> Unit)? = null,
) {
    val text = autostartSetupText(step, restartNeeded)
    Card(modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Setting up", style = MaterialTheme.typography.titleMedium)
            text.lead?.let { Text(it) }
            if (text.working) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                    Text(text.status)
                }
            } else if (text.question != null) {
                Text(text.status, style = MaterialTheme.typography.titleSmall)
            } else {
                Text(text.status, color = if (text.failed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
            }
            text.question?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            text.note?.let { Text(it, color = MaterialTheme.colorScheme.tertiary) }
            text.restart?.let { Text(it, color = MaterialTheme.colorScheme.tertiary) }
            (step as? AutostartStep.Done)?.restartError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            val showJob = onOpenJob != null && step is AutostartStep.Installing && step.planId != null
            // Only a set-up that has ended can be hidden: one that asks waits for its answer.
            val hide = onDismiss != null && (step is AutostartStep.Done || step is AutostartStep.Restarting || step is AutostartStep.Failed)
            if (text.question != null) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    Button(onClick = onRemove) { Text("${text.fixLabel}…") }
                    TextButton(onClick = onSkip) { Text("Skip") }
                }
            }
            if (text.failed || text.restart != null || showJob || hide) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    if (text.failed) Button(onClick = onRetry) { Text("Retry") }
                    if (text.restart != null) Button(onClick = onRestart) { Text("Restart iDrive now") }
                    if (showJob) TextButton(onClick = { onOpenJob?.invoke() }) { Text("Show progress") }
                    if (hide) TextButton(onClick = { onDismiss?.invoke() }) { Text("Hide") }
                }
            }
        }
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.dp
import com.nbtapps.store.R
import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.AutostartStage
import com.nbtapps.store.data.jobs.AutostartStep

/** Where one row of the checklist stands. */
enum class ChecklistMark {
    DONE,
    WORKING,

    /** Still to come: the set-up runs, and will go on to it. */
    TO_COME,

    /** The set-up stopped here. */
    FAILED,
}

/** One row of the checklist: the set-up's [stage], what the row says, and where it stands. */
data class ChecklistRow(val stage: AutostartStage, val text: String, val mark: ChecklistMark)

/**
 * What the checklist of a set-up shows: see [autostartChecklist]. [rows], in order, then [end] under them where the
 * set-up has ended or asks a question: what the set-up's card would say then ([autostartSetupText]).
 */
internal data class AutostartChecklistText(val rows: List<ChecklistRow>, val end: AutostartSetupText?)

/** The step whose card text is what [stage]'s row says while it runs: the card's own words for it. */
private fun stepOf(stage: AutostartStage): AutostartStep = when (stage) {
    AutostartStage.CHECKING -> AutostartStep.Checking
    AutostartStage.GETTING_PLUGIN -> AutostartStep.Downloading
    AutostartStage.INSTALLING_PLUGIN -> AutostartStep.Installing()
    AutostartStage.SWITCHING_ON -> AutostartStep.SwitchingOn
}

/** What [stage]'s row says while it runs: "Installing the iDrive plugin…", as the set-up's card says it. */
private fun workingText(stage: AutostartStage): String = autostartSetupText(stepOf(stage), restartNeeded = false).status

/** What [stage]'s row says once done. */
internal fun doneText(stage: AutostartStage): String = when (stage) {
    AutostartStage.CHECKING -> "Checked the head unit"
    AutostartStage.GETTING_PLUGIN -> "Got the iDrive plugin"
    AutostartStage.INSTALLING_PLUGIN -> "Installed the iDrive plugin"
    AutostartStage.SWITCHING_ON -> "Switched on"
}

/** What [stage]'s row says while it is to come, or where it failed: the running words without the dots. */
private fun pendingText(stage: AutostartStage): String = workingText(stage).removeSuffix("…")

private fun row(stage: AutostartStage, mark: ChecklistMark) = ChecklistRow(
    stage,
    when (mark) {
        ChecklistMark.DONE -> doneText(stage)
        ChecklistMark.WORKING -> workingText(stage)
        ChecklistMark.TO_COME, ChecklistMark.FAILED -> pendingText(stage)
    },
    mark,
)

/**
 * What the checklist of [setup] shows, with [restartNeeded] as for [autostartSetupText]: a row for each stage the
 * run has begun ([AutostartSetupState.stages]), done, or the last one working or failed; and while it runs, the
 * stages it will still go through, to come. Which stages a run needs is known only as it goes: the plugin's rows
 * come once it turns out to be wanting. A stage the run did not go through has no row, so a set-up that only
 * switches on, or finds the agent switched on already, never shows the plugin as got or installed.
 *
 * Under the rows, [AutostartChecklistText.end] is what the set-up's card says once the run has ended or asks about
 * what was installed by hand: the closing text, the failure's reason, the question and its fix, or the fix that runs.
 */
internal fun autostartChecklist(setup: AutostartSetupState, restartNeeded: Boolean): AutostartChecklistText {
    val stages = setup.stages.ifEmpty { listOf(AutostartStage.CHECKING) }
    return when (val step = setup.step) {
        AutostartStep.Checking, AutostartStep.Downloading, is AutostartStep.Installing, AutostartStep.SwitchingOn -> {
            val now = stages.last()
            val toCome = when (now) {
                // Whether anything comes after is known only once the head unit has been looked at.
                AutostartStage.CHECKING -> emptyList()
                else -> AutostartStage.entries.filter { it > now }
            }
            AutostartChecklistText(
                stages.dropLast(1).map { row(it, ChecklistMark.DONE) } + row(now, ChecklistMark.WORKING) + toCome.map { row(it, ChecklistMark.TO_COME) },
                end = null,
            )
        }
        is AutostartStep.Failed -> AutostartChecklistText(
            stages.dropLast(1).map { row(it, ChecklistMark.DONE) } + row(stages.last(), ChecklistMark.FAILED),
            autostartSetupText(step, restartNeeded),
        )
        // Done, the restart, and the questions about what was installed by hand: every stage begun is behind it.
        else -> AutostartChecklistText(stages.map { row(it, ChecklistMark.DONE) }, autostartSetupText(step, restartNeeded))
    }
}

/**
 * The set-up of [setup] as a checklist, for a page of its own: a row for each stage with a mark for where it stands
 * ([autostartChecklist]), then, once the set-up has ended or asks, what the set-up's card says then, with the same
 * buttons: the fix's button and Skip where it asks about what was installed by hand ([onRemove] opens the
 * confirmation, [CheckFixDialog]), Retry where it stopped short, and "Restart iDrive now" once it is done where
 * iDrive has to restart ([restartNeeded], as for [autostartSetupText]).
 */
@Composable
fun AutostartChecklist(
    setup: AutostartSetupState,
    restartNeeded: Boolean,
    onRetry: () -> Unit,
    onRestart: () -> Unit,
    onRemove: () -> Unit,
    onSkip: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val checklist = autostartChecklist(setup, restartNeeded)
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Column {
            checklist.rows.forEach { ChecklistLine(it) }
        }
        checklist.end?.let { End(it, setup.step, onRetry, onRestart, onRemove, onSkip) }
    }
}

@Composable
private fun ChecklistLine(row: ChecklistRow) {
    val colors = MaterialTheme.colorScheme
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .semantics(mergeDescendants = true) { stateDescription = markDescription(row.mark) },
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Box(Modifier.size(24.dp), contentAlignment = Alignment.Center) {
            when (row.mark) {
                ChecklistMark.DONE -> Icon(painterResource(R.drawable.ic_check), contentDescription = null, tint = colors.primary)
                ChecklistMark.WORKING -> CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                ChecklistMark.TO_COME -> Icon(painterResource(R.drawable.ic_to_come), contentDescription = null, tint = colors.outline)
                ChecklistMark.FAILED -> Icon(painterResource(R.drawable.ic_error), contentDescription = null, tint = colors.error)
            }
        }
        Text(
            row.text,
            style = MaterialTheme.typography.bodyLarge,
            color = when (row.mark) {
                ChecklistMark.TO_COME -> colors.onSurfaceVariant
                ChecklistMark.FAILED -> colors.error
                else -> colors.onSurface
            },
        )
    }
}

/** What a screen reader says of a row's mark, which is otherwise only drawn. */
private fun markDescription(mark: ChecklistMark): String = when (mark) {
    ChecklistMark.DONE -> "Done"
    ChecklistMark.WORKING -> "Under way"
    ChecklistMark.TO_COME -> "To come"
    ChecklistMark.FAILED -> "Failed"
}

/** What the set-up's card says under the rows, as it says it: see [AutostartSetupCard]. */
@Composable
private fun End(
    text: AutostartSetupText,
    step: AutostartStep,
    onRetry: () -> Unit,
    onRestart: () -> Unit,
    onRemove: () -> Unit,
    onSkip: () -> Unit,
) {
    val colors = MaterialTheme.colorScheme
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        text.lead?.let { Text(it) }
        when {
            text.working -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                Text(text.status)
            }
            text.question != null -> Text(text.status, style = MaterialTheme.typography.titleSmall)
            // A failure's reason: the failed row says where.
            else -> Text(text.status, color = if (text.failed) colors.error else colors.onSurface)
        }
        text.question?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = colors.onSurfaceVariant) }
        text.note?.let { Text(it, color = colors.tertiary) }
        text.restart?.let { Text(it, color = colors.tertiary) }
        (step as? AutostartStep.Done)?.restartError?.let { Text(it, color = colors.error) }
        if (text.question != null) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Button(onClick = onRemove) { Text("${text.fixLabel}…") }
                TextButton(onClick = onSkip) { Text("Skip") }
            }
        }
        if (text.failed) Button(onClick = onRetry) { Text("Retry") }
        if (text.restart != null) Button(onClick = onRestart) { Text("Restart iDrive now") }
    }
}

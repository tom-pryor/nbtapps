// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.nbtapps.store.R
import com.nbtapps.store.ui.headunits.ChecklistMark
import com.nbtapps.store.ui.headunits.ChecklistMarkIcon
import com.nbtapps.store.ui.headunits.CopyDiagnosticsButton
import com.nbtapps.store.ui.headunits.markDescription

/** The first row of the Connect page, where the car is in reach over Bluetooth. */
internal const val CAR_IN_REACH = "The car is in reach over Bluetooth"

/** The first row, where it is not. */
internal const val CAR_NOT_IN_REACH = "The car isn't connected to this phone over Bluetooth. Is the ignition on?"

/** What [ConnectPage]'s "What to check" card says, as a title and its detail for each item. */
internal val WHAT_TO_CHECK: List<Pair<String, String>> = listOf(
    "Is the agent running?" to
        "On the car's screen: NBTApp, then Start the agent. On a car without the plugin, start it over SSH, as in step 3 of the install guide.",
    "Has the car only just started?" to "Give it half a minute and try again.",
    "Is wireless Android Auto on?" to
        "It can hold the car's one link for phone apps. With a recent agent it can stay on: show the pairing code on the " +
        "car (NBTApp, then Pair a phone; on a car without the plugin, nbtctl pair over SSH, as in step 5 of the install " +
        "guide) and keep this page open. The car connects to this app by itself within a few seconds, and the set-up " +
        "goes on. With an older agent, wait until Android Auto is up, then try again.",
)

/**
 * Step 2, the link to the agent on the car, which was begun when the car was picked: no tap starts it. While it is
 * opened, [failure] is null, and Cancel ([onCancel], as back) gives it up. Once it failed, [failure] says why, and
 * the page says what to check, with Try again ([onRetry]) and Pick another car ([onPickAnother]), and "Copy
 * diagnostics", which copies what [diagnostics] gives.
 *
 * The first row says whether the car is connected to the phone over Bluetooth at all, read again each time the page
 * comes back ([onRefresh]); a head unit with no Bluetooth endpoint has no such row.
 */
@Composable
internal fun ConnectPage(
    uiState: SetupUiState,
    failure: String?,
    onCancel: () -> Unit,
    onSkip: () -> Unit,
    onRetry: () -> Unit,
    onPickAnother: () -> Unit,
    onRefresh: () -> Unit,
    diagnostics: () -> String,
) {
    val context = LocalContext.current
    LifecycleResumeEffect(Unit) {
        onRefresh()
        onPauseOrDispose {}
    }
    SetupFrame(
        number = setupStepNumber(SetupStep.Connect()),
        count = uiState.stepCount,
        onBack = onCancel,
        onSkip = onSkip,
        buttons = {
            if (failure == null) {
                TextButton(onClick = onCancel, Modifier.fillMaxWidth()) { Text("Cancel") }
            } else {
                Button(onClick = onRetry, Modifier.fillMaxWidth()) { Text("Try again") }
                TextButton(onClick = onPickAnother, Modifier.fillMaxWidth()) { Text("Pick another car") }
            }
        },
    ) {
        PageTitle(if (failure == null) "Connecting to ${uiState.name}" else "Couldn't reach the agent")
        if (failure == null) MutedText("Keep the phone in the car, with the ignition on.")
        Column {
            uiState.carInReach?.let { inReach ->
                StatusRow(if (inReach) ChecklistMark.DONE else ChecklistMark.TO_COME, if (inReach) CAR_IN_REACH else CAR_NOT_IN_REACH)
            }
            if (failure == null) {
                StatusRow(
                    ChecklistMark.WORKING,
                    "Opening a link to the agent…",
                    note = "The agent starts a little after iDrive does. If the car has only just started, this can take half a minute.",
                )
            } else {
                StatusRow(ChecklistMark.FAILED, "No agent answered on ${uiState.name}", note = failure, noteColor = MaterialTheme.colorScheme.error)
            }
        }
        if (failure == null) {
            Card(Modifier.fillMaxWidth()) {
                Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.Top) {
                    Icon(painterResource(R.drawable.ic_info), contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(
                        "While the app is connected, the car maker's own phone features over Bluetooth serial don't work. Calls and music are not affected.",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
        } else {
            SectionHeading("What to check")
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(vertical = 4.dp)) {
                    WHAT_TO_CHECK.forEachIndexed { index, (title, detail) ->
                        if (index > 0) HorizontalDivider(Modifier.padding(horizontal = 16.dp))
                        CardRow(title, detail)
                    }
                }
            }
            TextButton(onClick = { openInstallGuide(context) }) { Text("Open the install guide") }
            CopyDiagnosticsButton(diagnostics)
        }
    }
}

/**
 * A row with a mark for where it stands, as the checklist of the set-up to start with the car draws them, and a
 * [note] under it in [noteColor].
 */
@Composable
internal fun StatusRow(mark: ChecklistMark, text: String, note: String? = null, noteColor: Color = Color.Unspecified) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .padding(vertical = 8.dp)
            .semantics(mergeDescendants = true) { stateDescription = markDescription(mark) },
        horizontalArrangement = Arrangement.spacedBy(16.dp),
        verticalAlignment = Alignment.Top,
    ) {
        ChecklistMarkIcon(mark)
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                text,
                style = MaterialTheme.typography.bodyLarge,
                color = when (mark) {
                    ChecklistMark.TO_COME -> MaterialTheme.colorScheme.onSurfaceVariant
                    ChecklistMark.FAILED -> MaterialTheme.colorScheme.error
                    else -> MaterialTheme.colorScheme.onSurface
                },
            )
            if (note != null) {
                Text(
                    note,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (noteColor == Color.Unspecified) MaterialTheme.colorScheme.onSurfaceVariant else noteColor,
                )
            }
        }
    }
}

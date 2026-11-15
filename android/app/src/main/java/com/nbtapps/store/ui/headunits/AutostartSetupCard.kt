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
    "The agent then starts with the car. The iDrive plugin does the starting, and is installed if the car doesn't have it."

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
)

/**
 * What the card of a set-up at [step] says. [restartNeeded]: the agent says over the live link that iDrive has to
 * restart, as it does once the plugin is installed; it is offered only once the set-up is done.
 */
internal fun autostartSetupText(step: AutostartStep, restartNeeded: Boolean): AutostartSetupText = when (step) {
    AutostartStep.Checking -> AutostartSetupText("Checking the head unit…", working = true)
    AutostartStep.Downloading -> AutostartSetupText("Getting the iDrive plugin…", working = true)
    is AutostartStep.Installing -> AutostartSetupText("Installing the iDrive plugin…", working = true)
    AutostartStep.SwitchingOn -> AutostartSetupText("Switching on…", working = true)
    is AutostartStep.Done -> AutostartSetupText(
        if (step.changed) "The agent now starts with the car." else "The agent already starts with the car.",
        restart = AUTOSTART_RESTART_NEEDED.takeIf { restartNeeded },
    )
    AutostartStep.Restarting -> AutostartSetupText(AUTOSTART_RESTARTING)
    is AutostartStep.Failed -> AutostartSetupText(step.reason, failed = true)
}

internal const val AUTOSTART_RESTART_NEEDED =
    "iDrive has to restart before it loads the plugin. That restarts the whole head unit, and the app loses its link."

internal const val AUTOSTART_RESTARTING =
    "iDrive is restarting. The agent starts about 15 seconds after it is back. Connect again then."

/**
 * The set-up at [step], on a card: where it stands, Retry where it stopped short, and "Restart iDrive now" once it is
 * done where iDrive has to restart ([restartNeeded], as for [autostartSetupText]). [onOpenJob], where the caller
 * offers it, leads to the job screen while the plugin installs; [onDismiss], where offered, hides a set-up that
 * has ended.
 */
@Composable
fun AutostartSetupCard(
    step: AutostartStep,
    restartNeeded: Boolean,
    onRetry: () -> Unit,
    onRestart: () -> Unit,
    modifier: Modifier = Modifier,
    onOpenJob: (() -> Unit)? = null,
    onDismiss: (() -> Unit)? = null,
) {
    val text = autostartSetupText(step, restartNeeded)
    Card(modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Start with the car", style = MaterialTheme.typography.titleMedium)
            if (text.working) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                    Text(text.status)
                }
            } else {
                Text(text.status, color = if (text.failed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
            }
            text.restart?.let { Text(it, color = MaterialTheme.colorScheme.tertiary) }
            (step as? AutostartStep.Done)?.restartError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            val showJob = onOpenJob != null && step is AutostartStep.Installing && step.planId != null
            val hide = onDismiss != null && !text.working
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

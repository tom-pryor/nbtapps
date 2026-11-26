// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.restart

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.size
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties

internal const val RESTART_TITLE = "Restart iDrive?"
internal const val RESTART_TEXT = "iDrive on the head unit restarts and the link to this phone ends. Tap Connect on Head units when it is back."

/**
 * The line under the dialog's text for what ends with the link besides: the terminal open on the head unit, the SSH
 * port for other apps, or both. None where neither is open.
 */
internal fun restartOpenLine(terminalOpen: Boolean, portOpen: Boolean): String? = when {
    terminalOpen && portOpen -> "The open terminal and the SSH port for other apps close too."
    terminalOpen -> "The open terminal closes too."
    portOpen -> "The SSH port for other apps closes too."
    else -> null
}

/**
 * The question before iDrive is restarted, at [step]: nothing while it is [RestartStep.Closed]. It asks with Cancel
 * and Restart; while the agent's answer is waited for it says Restarting… with both off, and Back or a tap beside it
 * does not close it; a refusal is in red under the text, with Cancel and Restart, which tries again; a link that ended
 * before the answer is in red too, with Close alone.
 */
@Composable
fun RestartDialog(step: RestartStep, onConfirm: () -> Unit, onCancel: () -> Unit) {
    val open = step as? RestartStep.Open ?: return
    val waiting = open is RestartStep.Waiting
    val ended = (open as? RestartStep.Failed)?.linkEnded == true
    AlertDialog(
        onDismissRequest = { if (!waiting) onCancel() },
        properties = DialogProperties(dismissOnBackPress = !waiting, dismissOnClickOutside = !waiting),
        title = { Text(RESTART_TITLE) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(RESTART_TEXT)
                restartOpenLine(open.terminalOpen, open.portOpen)?.let { Text(it) }
                (open as? RestartStep.Failed)?.let { Text(it.text, color = MaterialTheme.colorScheme.error) }
            }
        },
        confirmButton = {
            if (ended) {
                TextButton(onClick = onCancel) { Text("Close") }
            } else {
                TextButton(onClick = onConfirm, enabled = !waiting) { Text("Restart") }
            }
        },
        dismissButton = {
            if (!ended) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (waiting) {
                        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                        Text("Restarting…", color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.bodyMedium)
                    }
                    TextButton(onClick = onCancel, enabled = !waiting) { Text("Cancel") }
                }
            }
        },
    )
}

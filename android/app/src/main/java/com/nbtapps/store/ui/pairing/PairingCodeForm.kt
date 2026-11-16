// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairing

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.model.AgentText
import com.nbtapps.store.ui.headunits.AdvancedAutostart
import com.nbtapps.store.ui.headunits.ExpandRow

/** The title of the row that helps where the car's screen has no entry to pair a phone from. */
internal const val NO_ENTRY_HELP = "No NBTApp entry on the car's screen?"

/**
 * What the row says once opened: its text and commands, in order. A command is shown as such, in a monospace font;
 * the app cannot know the agent's version before the phone is paired, hence the two commands.
 */
internal val NO_ENTRY_HELP_TEXT: List<HelpLine> = listOf(
    HelpLine(
        "The iDrive plugin isn't on this car yet. Get a code over SSH instead, in a shell on the head unit. " +
            "First see which version of the agent is installed:",
    ),
    HelpLine("cat /var/opt/nbtapp/agent/current", command = true),
    HelpLine("Then ask it for a code, with that version in place of 1.4.0:"),
    HelpLine("/var/opt/nbtapp/agent/1.4.0/nbtctl pair", command = true),
    HelpLine("With autostart on, the app installs the plugin once the phone is paired, so this is needed only once."),
)

/** One line of [NO_ENTRY_HELP_TEXT]: a sentence, or a [command] to type in a shell. */
internal data class HelpLine(val text: String, val command: Boolean = false)

/**
 * Taking the code from the car's screen, wherever the app pairs the phone by one: where on the car the code is
 * shown, the field to type it in, and under it a row that opens help for a car whose screen has no entry to pair a
 * phone from yet, as the code can then be had over SSH. Then the autostart box under Advanced where
 * [offersAutostart], and last Pair, or while the code is with the agent a line that says so.
 *
 * [entry] is the code and how handing it over goes, from [CodePairing]; [onCode] is told what the user types, and
 * [onPair] the tap of Pair or the keyboard's Done. [enableAutostart] and [onEnableAutostart] are the box's. The
 * screen says before this why the phone is to be paired.
 */
@Composable
fun PairingCodeForm(
    entry: CodeEntry,
    offersAutostart: Boolean,
    enableAutostart: Boolean,
    onCode: (String) -> Unit,
    onEnableAutostart: (Boolean) -> Unit,
    onPair: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Text(AgentText.PAIR_ON_CAR)
        Text(
            "It shows ${ProtocolConstants.PAIRING_CODE_DIGITS} digits in two rows, good for " +
                "${ProtocolConstants.PAIRING_WINDOW_SECONDS / 60} minutes. Type them here, top row first. After " +
                "${ProtocolConstants.PAIRING_TRIES} wrong codes the car shows none for ${ProtocolConstants.PAIRING_PAUSE_SECONDS} seconds.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        OutlinedTextField(
            value = entry.code,
            onValueChange = onCode,
            label = { Text("Code from the car's screen") },
            singleLine = true,
            enabled = !entry.working,
            isError = entry.error != null,
            supportingText = entry.error?.let { error -> { Text(error) } },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Done),
            keyboardActions = KeyboardActions(onDone = { onPair() }),
            modifier = Modifier.fillMaxWidth(),
        )
        NoEntryHelp()
        if (offersAutostart) AdvancedAutostart(enableAutostart, onEnableAutostart, enabled = !entry.working)
        if (entry.working) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                Text("Pairing…")
            }
        } else {
            Button(onClick = onPair, enabled = entry.complete) { Text("Pair") }
        }
    }
}

/** [NO_ENTRY_HELP], closed to begin with; the commands can be selected, to copy them. */
@Composable
private fun NoEntryHelp() {
    var open by rememberSaveable { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        ExpandRow(NO_ENTRY_HELP, line = null, open = open, onToggle = { open = !open })
        if (open) {
            NO_ENTRY_HELP_TEXT.forEach { line ->
                if (line.command) {
                    Surface(color = MaterialTheme.colorScheme.surfaceContainerHigh, shape = MaterialTheme.shapes.small) {
                        SelectionContainer {
                            Text(
                                line.text,
                                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                                fontFamily = FontFamily.Monospace,
                                style = MaterialTheme.typography.bodyMedium,
                            )
                        }
                    }
                } else {
                    Text(line.text, style = MaterialTheme.typography.bodyMedium)
                }
            }
        }
    }
}

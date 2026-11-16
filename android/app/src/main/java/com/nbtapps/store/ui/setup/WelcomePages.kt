// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.nbtapps.store.R

/**
 * The first page: what the app does, the two things it needs, and whether the agent is on the car yet. [onYes] goes
 * on to picking the car, [onNotYet] to how the agent is installed, and [onBrowse] skips the set-up.
 */
@Composable
internal fun WelcomePage(onYes: () -> Unit, onNotYet: () -> Unit, onBrowse: () -> Unit) {
    SetupFrame(
        buttons = {
            Button(onClick = onYes, Modifier.fillMaxWidth()) { Text("Yes, it's installed") }
            OutlinedButton(onClick = onNotYet, Modifier.fillMaxWidth()) { Text("Not yet") }
            TextButton(onClick = onBrowse, Modifier.fillMaxWidth()) { Text("Just browse the catalogue") }
        },
    ) {
        PageIcon(R.drawable.ic_head_unit, Modifier.padding(top = 24.dp))
        PageTitle("Set up your car")
        MutedText("NBTApps installs apps on your car's head unit from this phone, over Bluetooth.")
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(vertical = 4.dp)) {
                CardRow("The NBTApp agent on the head unit", "Installed once, from a computer, over SSH.") { RowNumber(1) }
                CardRow("This phone paired with the car", "In the phone's Bluetooth settings, as for calls or music.") { RowNumber(2) }
            }
        }
        Text("Is the agent on your car yet?", style = MaterialTheme.typography.titleMedium)
    }
}

/**
 * The page for a car the agent is not on yet: the app cannot put it there, and the install guide can. [onBack] goes
 * back to the question, and [onInstalled] on to picking the car.
 */
@Composable
internal fun NotYetPage(onBack: () -> Unit, onInstalled: () -> Unit) {
    val context = LocalContext.current
    SetupFrame(
        onBack = onBack,
        buttons = {
            Button(onClick = { sendInstallGuide(context) }, Modifier.fillMaxWidth()) { Text("Send the guide to my computer") }
            OutlinedButton(onClick = { openInstallGuide(context) }, Modifier.fillMaxWidth()) { Text("Read it on this phone") }
            TextButton(onClick = onInstalled, Modifier.fillMaxWidth()) { Text("The agent is installed now") }
        },
    ) {
        PageTitle("Install the agent first")
        MutedText("This part is done from a computer, once per car. The app can't do it for you. Once the agent is on the car, the app does the rest.")
        SectionHeading("What it takes")
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(vertical = 4.dp)) {
                CardRow("A car with an NBT Evo head unit, and a root shell on it over SSH")
                HorizontalDivider(Modifier.padding(horizontal = 16.dp))
                CardRow("A computer, to download the agent and reach the car")
                HorizontalDivider(Modifier.padding(horizontal = 16.dp))
                CardRow("A USB stick, or SSH from the computer, to put the files on the car")
            }
        }
        SectionHeading("The install guide")
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("Steps 1 to 3 put the agent on the car and start it. Come back here after step 3.", style = MaterialTheme.typography.bodyLarge)
                SelectionContainer {
                    Text(
                        INSTALL_GUIDE_URL,
                        Modifier.padding(top = 8.dp),
                        fontFamily = FontFamily.Monospace,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

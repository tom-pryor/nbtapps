// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
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
                CardRow("The NBTApp agent on the head unit", "Installed once, before the app can reach the car.") { RowNumber(1) }
                CardRow("This phone paired with the car", "In the phone's Bluetooth settings, as for calls or music.") { RowNumber(2) }
            }
        }
        Text("Is the agent on your car yet?", style = MaterialTheme.typography.titleMedium)
    }
}

/**
 * The page for a car the agent is not on yet: the app cannot put it there, and the page gives the two ways that can.
 * The first is the installer, a web page that puts the agent on the car by itself; the second is the install guide,
 * followed by hand. [onBack] goes back to the question, and [onInstalled] on to picking the car.
 */
@Composable
internal fun NotYetPage(onBack: () -> Unit, onInstalled: () -> Unit) {
    val context = LocalContext.current
    SetupFrame(
        onBack = onBack,
        buttons = {
            OutlinedButton(onClick = onInstalled, Modifier.fillMaxWidth()) { Text("The agent is installed now") }
        },
    ) {
        PageTitle("Install the agent first")
        MutedText("This is done once per car, before the app can reach it. There are two ways to do it.")

        SectionHeading("Option 1: the installer")
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(vertical = 4.dp)) {
                CardRow("It installs the agent for you", "A web page that works on an Android phone, or on a computer.")
                HorizontalDivider(Modifier.padding(horizontal = 16.dp))
                CardRow("You need an ENET cable", "And a USB Ethernet adapter, if your phone or computer has no Ethernet port.")
                Column(Modifier.padding(start = 16.dp, end = 16.dp, bottom = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Address(AGENT_INSTALLER_URL)
                    Button(onClick = { openAgentInstaller(context) }, Modifier.fillMaxWidth()) { Text("Open the installer") }
                }
            }
        }

        SectionHeading("Option 2: by hand")
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(vertical = 4.dp)) {
                CardRow("Follow the install guide", "Steps 1 to 3 put the agent on the car and start it.")
                HorizontalDivider(Modifier.padding(horizontal = 16.dp))
                CardRow("You need SSH access to the head unit", "The installer in option 1 can switch it on.")
                Column(Modifier.padding(start = 16.dp, end = 16.dp, bottom = 12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Address(INSTALL_GUIDE_URL)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(onClick = { openInstallGuide(context) }) { Text("Read the guide") }
                        TextButton(onClick = { sendInstallGuide(context) }) { Text("Send it to my computer") }
                    }
                }
            }
        }

        MutedText("Come back here once the agent is on the car.")
    }
}

/** A web address in small type, which can be selected to copy it. */
@Composable
private fun Address(url: String) {
    SelectionContainer {
        Text(
            url,
            fontFamily = FontFamily.Monospace,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

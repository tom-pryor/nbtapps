// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.nbtapps.store.R
import com.nbtapps.store.data.jobs.AutostartSetupState
import com.nbtapps.store.data.jobs.AutostartStage
import com.nbtapps.store.data.jobs.AutostartStep
import com.nbtapps.store.ui.headunits.AUTOSTART_RESTARTING
import com.nbtapps.store.ui.headunits.AutostartChecklist
import com.nbtapps.store.ui.headunits.ChecklistMark
import com.nbtapps.store.ui.headunits.autostartSetupText
import com.nbtapps.store.ui.headunits.startsText

/**
 * The button that moves on from the start-with-the-car page, by how [setup] stands: "Not now" while iDrive's restart
 * is offered, "Continue" once the set-up has ended otherwise, and none while it runs or asks. [restartNeeded] is as
 * for [autostartSetupText].
 */
internal fun moveOnLabel(setup: AutostartSetupState, restartNeeded: Boolean): String? = when {
    autostartSetupText(setup.step, restartNeeded).restart != null -> "Not now"
    setup.ended -> "Continue"
    else -> null
}

/**
 * Whether the agent on the car starts with it, as far as the set-up has seen: [setup] switched it on or found it
 * on, or with no set-up, the link last said so ([startsWithCar]).
 */
internal fun startsWithCarNow(setup: AutostartSetupState?, startsWithCar: Boolean): Boolean = when (setup?.step) {
    is AutostartStep.Done, AutostartStep.Restarting -> true
    null -> startsWithCar
    else -> false
}

/**
 * What the Done page says of the head unit [name], by how [setup] stands: iDrive restarting, iDrive still to be
 * restarted ([restartNeeded], or the plugin installed by this run), the agent starting with the car, a set-up that
 * failed or is not over, or no set-up, where the agent starts with the car or not as the link last said
 * ([startsWithCar]).
 */
internal fun doneText(name: String, setup: AutostartSetupState?, restartNeeded: Boolean, startsWithCar: Boolean): String {
    val step = setup?.step
    return when {
        step == AutostartStep.Restarting -> AUTOSTART_RESTARTING
        step is AutostartStep.Done && (restartNeeded || AutostartStage.INSTALLING_PLUGIN in setup.stages) ->
            "iDrive has to restart before the agent starts with the car. Restart it from $name's page when it suits."
        step is AutostartStep.Done -> startsText(step.changed)
        step is AutostartStep.Failed -> "Setting the car up to start the agent didn't finish. $name's page shows where it stands."
        step != null -> "Setting the car up to start the agent goes on by itself. $name's page shows where it stands."
        startsWithCar -> startsText(switchedOn = false)
        else -> "Autostart is off: start the agent on the car each time before you connect."
    }
}

/**
 * Step 4, the car being set up to start the agent with it: the set-up's checklist ([AutostartChecklist]), wired as
 * the set-up's card is on the pairing screen, and a button to move on ([moveOnLabel]). It has no back arrow: back
 * leaves the set-up as Skip does, and the set-up goes on by itself.
 */
@Composable
internal fun StartWithCarPage(
    uiState: SetupUiState,
    onSkip: () -> Unit,
    onRetry: () -> Unit,
    onRestart: () -> Unit,
    onRemove: () -> Unit,
    onSkipRemoval: () -> Unit,
    onMoveOn: () -> Unit,
) {
    val setup = uiState.setup
    SetupFrame(
        number = setupStepNumber(SetupStep.StartWithCar),
        // Always the last of four: the page is there only where the car is being set up.
        count = 4,
        onSkip = onSkip,
        buttons = {
            setup?.let { moveOnLabel(it, uiState.restartNeeded) }?.let { label ->
                TextButton(onClick = onMoveOn, Modifier.fillMaxWidth()) { Text(label) }
            }
        },
    ) {
        PageTitle("Start with the car")
        MutedText("This phone is paired with ${uiState.name}. The app is now setting the car up to start the agent by itself.")
        if (setup != null) {
            AutostartChecklist(
                setup = setup,
                restartNeeded = uiState.restartNeeded,
                onRetry = onRetry,
                onRestart = onRestart,
                onRemove = onRemove,
                onSkip = onSkipRemoval,
            )
        }
    }
}

/**
 * The last page: the car is set up, as far as it went ([doneText]), and from here on the catalogue is where packages
 * are installed. [onBrowse] and [onOpenHeadUnit] leave the set-up, for the catalogue or the head unit's page.
 */
@Composable
internal fun DonePage(uiState: SetupUiState, onBrowse: () -> Unit, onOpenHeadUnit: () -> Unit) {
    SetupFrame(
        buttons = {
            Button(onClick = onBrowse, Modifier.fillMaxWidth()) { Text("Browse the catalogue") }
            TextButton(onClick = onOpenHeadUnit, Modifier.fillMaxWidth()) { Text("Open ${uiState.name}") }
        },
    ) {
        PageIcon(R.drawable.ic_check, Modifier.padding(top = 24.dp))
        PageTitle("${uiState.name} is set up")
        MutedText(doneText(uiState.name, uiState.setup, uiState.restartNeeded, uiState.startsWithCar))
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                StatusRow(ChecklistMark.DONE, "Car added")
                StatusRow(ChecklistMark.DONE, "Phone paired")
                if (startsWithCarNow(uiState.setup, uiState.startsWithCar)) StatusRow(ChecklistMark.DONE, "Agent starts with the car")
            }
        }
        MutedText("From here on, packages are installed, updated and removed from the catalogue. The car is under Head units.")
    }
}

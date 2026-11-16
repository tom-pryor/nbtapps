// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.ui.headunits.AddHeadUnitFlow
import com.nbtapps.store.ui.headunits.CarPickerFlow

/**
 * Step 1, picking the car among the devices paired with the phone ([CarPickerFlow]), then Connect. [onSelect] is
 * told the device picked in the list, and [onConnect] the tap of Connect.
 *
 * Where the build reaches a head unit at a network address ([SetupUiState.takesAddress]), a button opens the add
 * dialog ([AddHeadUnitFlow]) as well: [onBeginAddress] is told as it opens, and [onAdded] the head unit it saved,
 * which the set-up goes on with.
 */
@Composable
internal fun PickCarPage(
    uiState: SetupUiState,
    onBack: () -> Unit,
    onSkip: () -> Unit,
    onSelect: (PairedDevice) -> Unit,
    onConnect: () -> Unit,
    onBeginAddress: () -> Unit,
    onAdded: (String) -> Unit,
) {
    var addingAddress by rememberSaveable { mutableStateOf(false) }
    val selected = uiState.selected
    SetupFrame(
        number = setupStepNumber(SetupStep.PickCar),
        count = uiState.stepCount,
        onBack = onBack,
        onSkip = onSkip,
        buttons = {
            uiState.saveError?.let { Text(it, color = MaterialTheme.colorScheme.error, textAlign = TextAlign.Center) }
            if (selected != null) {
                Text(
                    "The app will call it ${selected.name}.",
                    Modifier.fillMaxWidth(),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                )
            }
            Button(onClick = onConnect, Modifier.fillMaxWidth(), enabled = selected != null && !uiState.saving) {
                Text(if (selected != null) "Connect to ${selected.name}" else "Connect")
            }
            if (uiState.takesAddress) {
                TextButton(
                    onClick = {
                        onBeginAddress()
                        addingAddress = true
                    },
                    Modifier.fillMaxWidth(),
                ) { Text("Use a network address") }
            }
        },
    ) {
        PageTitle("Which car?")
        MutedText("The cars paired with this phone. The one it is connected to now comes first.")
        CarPickerFlow(selected = selected?.address, onPick = onSelect)
    }

    if (uiState.takesAddress) {
        AddHeadUnitFlow(
            show = addingAddress,
            onDismiss = { addingAddress = false },
            onAdded = { id ->
                addingAddress = false
                onAdded(id)
            },
        )
    }
}

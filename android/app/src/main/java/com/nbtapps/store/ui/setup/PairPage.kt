// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.setup

import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import com.nbtapps.store.ui.pairing.PairingCodeForm
import com.nbtapps.store.ui.pairing.staleKeyText

/**
 * Step 3, pairing the phone by the code the car's screen shows ([PairingCodeForm]), over the link that waits for it.
 * Where the agent no longer takes the phone's key ([SetupStep.Pair.stale]), the page says so first, as the pairing
 * screen does. The autostart box is offered where the agent can be started with the car; its tick is the set-up's,
 * kept in the saved state, and ticked to begin with.
 */
@Composable
internal fun PairPage(
    uiState: SetupUiState,
    step: SetupStep.Pair,
    onBack: () -> Unit,
    onSkip: () -> Unit,
    onCode: (String) -> Unit,
    onEnableAutostart: (Boolean) -> Unit,
    onPair: () -> Unit,
) {
    SetupFrame(number = setupStepNumber(step), count = uiState.stepCount, onBack = onBack, onSkip = onSkip) {
        PageTitle("Pair this phone")
        if (step.stale) Text(staleKeyText(uiState.name))
        PairingCodeForm(
            entry = uiState.code,
            offersAutostart = step.offersAutostart,
            enableAutostart = uiState.enableAutostart,
            onCode = onCode,
            onEnableAutostart = onEnableAutostart,
            onPair = onPair,
        )
    }
}

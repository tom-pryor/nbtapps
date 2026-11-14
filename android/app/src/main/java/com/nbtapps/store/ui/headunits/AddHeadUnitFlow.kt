// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/**
 * Adding a head unit, wherever the app offers it: the dialog while [show], the Nearby devices permission it may
 * ask for, and the phone's Bluetooth read again on each return to the screen. [onAdded] gets the new head unit's
 * id once the form was valid; the dialog is the caller's to close then, as on [onDismiss].
 *
 * Kept in composition while the dialog is closed too, so the paired devices are current when it opens.
 */
@Composable
fun AddHeadUnitFlow(
    show: Boolean,
    onDismiss: () -> Unit,
    onAdded: (String) -> Unit,
    viewModel: HeadUnitsViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        viewModel.refreshBluetooth()
    }
    // Pairing a device or switching Bluetooth on happens outside the app.
    LifecycleResumeEffect(Unit) {
        viewModel.refreshBluetooth()
        onPauseOrDispose {}
    }

    if (show) {
        AddHeadUnitDialog(
            bluetooth = uiState.bluetooth,
            takesAddress = uiState.takesAddress,
            onAllowBluetooth = { permissionLauncher.launch(Manifest.permission.BLUETOOTH_CONNECT) },
            onDismiss = onDismiss,
            onConfirm = { form -> viewModel.addHeadUnit(form)?.let(onAdded) },
        )
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.spptest

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.model.EchoReport
import com.nbtapps.store.model.EchoSize
import com.nbtapps.store.model.PairedDevice
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SppTestScreen(
    onBack: () -> Unit,
    viewModel: SppTestViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        viewModel.refreshDevices()
    }
    // Pairing a device or switching Bluetooth on happens outside the app.
    LifecycleResumeEffect(Unit) {
        viewModel.refreshDevices()
        onPauseOrDispose {}
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Bluetooth serial test") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Text(
                "Sends data to the echo service on the head unit over a Bluetooth serial link and checks what " +
                    "comes back. Start the echo tool on the head unit first.",
                style = MaterialTheme.typography.bodyMedium,
            )
            if (!uiState.permissionGranted) {
                PermissionCard(onAllow = { permissionLauncher.launch(Manifest.permission.BLUETOOTH_CONNECT) })
                return@Column
            }
            DeviceCard(uiState.devices, uiState.selectedAddress, viewModel::selectDevice)
            SettingsCard(uiState, viewModel::setUuid, viewModel::setSize)
            Button(onClick = viewModel::runEcho, enabled = uiState.canRun) { Text("Run echo test") }
            RunStatus(uiState.run)
        }
    }
}

@Composable
private fun PermissionCard(onAllow: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Bluetooth access", style = MaterialTheme.typography.titleMedium)
            Text("The test needs the Nearby devices permission to list paired devices and connect to the car.")
            OutlinedButton(onClick = onAllow) { Text("Allow") }
        }
    }
}

@Composable
private fun DeviceCard(devices: List<PairedDevice>, selectedAddress: String?, onSelect: (String) -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Paired device", style = MaterialTheme.typography.titleMedium)
            if (devices.isEmpty()) {
                Text("No paired devices. Switch Bluetooth on and pair the phone with the car.")
            }
            Column(Modifier.selectableGroup()) {
                devices.forEach { device ->
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .selectable(
                                selected = device.address == selectedAddress,
                                onClick = { onSelect(device.address) },
                                role = Role.RadioButton,
                            )
                            .padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = device.address == selectedAddress, onClick = null)
                        Text(device.name, Modifier.padding(start = 8.dp))
                    }
                }
            }
        }
    }
}

@Composable
private fun SettingsCard(uiState: SppTestUiState, onUuidChange: (String) -> Unit, onSizeChange: (EchoSize) -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Test", style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(
                value = uiState.uuid,
                onValueChange = onUuidChange,
                label = { Text("Service UUID") },
                isError = uiState.uuidError != null,
                supportingText = uiState.uuidError?.let { { Text(it) } },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                EchoSize.entries.forEach { size ->
                    FilterChip(
                        selected = uiState.size == size,
                        onClick = { onSizeChange(size) },
                        label = { Text(size.label) },
                    )
                }
            }
        }
    }
}

@Composable
private fun RunStatus(run: EchoRunState) {
    when (run) {
        EchoRunState.Idle -> Unit
        is EchoRunState.Running -> Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(
                if (run.received == 0) "Connecting…" else "${run.received / 1024} of ${run.total / 1024} KB echoed",
            )
            LinearProgressIndicator(
                progress = { run.received.toFloat() / run.total },
                modifier = Modifier.fillMaxWidth(),
            )
        }
        is EchoRunState.Failed -> Text("Failed: ${run.reason}", color = MaterialTheme.colorScheme.error)
        is EchoRunState.Done -> ReportCard(run.report)
    }
}

@Composable
private fun ReportCard(report: EchoReport) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (report.intact) {
                Text(
                    "All ${report.bytesReceived} bytes came back unchanged",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
            } else {
                Text(
                    "Data came back changed, first at byte ${report.firstMismatchAt}",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            Text("First round trip: ${report.roundTripMillis} ms")
            Text("Total time: ${String.format(Locale.US, "%.1f", report.totalMillis / 1000.0)} s")
            Text("Echo rate: ${String.format(Locale.US, "%.1f", report.kiloBytesPerSecond)} KB/s")
            Text(
                "Each byte crosses the link twice, so one direction alone can be faster.",
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

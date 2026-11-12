// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.DebugTools
import com.nbtapps.store.R
import com.nbtapps.store.model.HeadUnit

@Composable
fun HeadUnitsScreen(
    onOpenHeadUnit: (String) -> Unit,
    onOpenSppTest: () -> Unit,
    viewModel: HeadUnitsViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    var showAddDialog by rememberSaveable { mutableStateOf(false) }
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        viewModel.refreshBluetooth()
    }
    // Pairing a device or switching Bluetooth on happens outside the app.
    LifecycleResumeEffect(Unit) {
        viewModel.refreshBluetooth()
        onPauseOrDispose {}
    }

    HeadUnitsContent(
        uiState = uiState,
        onOpenHeadUnit = onOpenHeadUnit,
        // The echo test is a tool of the test bench: the screen is in every build, the way to it in the debug one.
        onOpenSppTest = onOpenSppTest.takeIf { DebugTools.BLUETOOTH_TEST },
        onRemoveHeadUnit = viewModel::removeHeadUnit,
        onAddClick = { showAddDialog = true },
    )

    if (showAddDialog) {
        AddHeadUnitDialog(
            bluetooth = uiState.bluetooth,
            takesAddress = uiState.takesAddress,
            onAllowBluetooth = { permissionLauncher.launch(Manifest.permission.BLUETOOTH_CONNECT) },
            onDismiss = { showAddDialog = false },
            onConfirm = { form -> if (viewModel.addHeadUnit(form)) showAddDialog = false },
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun HeadUnitsContent(
    uiState: HeadUnitsUiState,
    onOpenHeadUnit: (String) -> Unit,
    onOpenSppTest: (() -> Unit)?,
    onRemoveHeadUnit: (String) -> Unit,
    onAddClick: () -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Head units") },
                actions = { onOpenSppTest?.let { TextButton(onClick = it) { Text("Bluetooth test") } } },
            )
        },
        floatingActionButton = {
            FloatingActionButton(onClick = onAddClick) {
                Icon(painterResource(R.drawable.ic_add), contentDescription = "Add head unit")
            }
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            if (uiState.headUnits.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        "No head units yet. Tap + to add one.",
                        style = MaterialTheme.typography.bodyLarge,
                    )
                }
            } else {
                LazyColumn {
                    items(uiState.headUnits, key = HeadUnit::id) { unit ->
                        HeadUnitRow(unit, onClick = { onOpenHeadUnit(unit.id) }, onRemove = { onRemoveHeadUnit(unit.id) })
                    }
                }
            }
        }
    }
}

@Composable
private fun HeadUnitRow(unit: HeadUnit, onClick: () -> Unit, onRemove: () -> Unit) {
    ListItem(
        modifier = Modifier.clickable(onClick = onClick),
        headlineContent = { Text(unit.name) },
        supportingContent = { Text(unit.endpoints.joinToString("\n") { it.label }) },
        trailingContent = { TextButton(onClick = onRemove) { Text("Remove") } },
    )
}

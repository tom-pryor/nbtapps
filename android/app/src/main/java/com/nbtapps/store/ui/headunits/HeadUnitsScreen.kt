// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.DebugTools
import com.nbtapps.store.R
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.ui.about.AboutButton

/**
 * The saved head units. With none, it offers the set-up ([onSetUp]); "+" opens the set-up for a further car
 * ([onAddCar]), which begins at picking it. The gear at the right of the top bar opens the app's settings
 * ([onOpenSettings]).
 */
@Composable
fun HeadUnitsScreen(
    onOpenHeadUnit: (String) -> Unit,
    onOpenSppTest: () -> Unit,
    onSetUp: () -> Unit,
    onAddCar: () -> Unit,
    onOpenSettings: () -> Unit,
    viewModel: HeadUnitsViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    HeadUnitsContent(
        uiState = uiState,
        onOpenHeadUnit = onOpenHeadUnit,
        // The echo test is a tool of the test bench: the screen is in every build, the way to it in the debug one.
        onOpenSppTest = onOpenSppTest.takeIf { DebugTools.BLUETOOTH_TEST },
        onRemoveHeadUnit = viewModel::removeHeadUnit,
        onSetUp = onSetUp,
        onAddClick = onAddCar,
        onOpenSettings = onOpenSettings,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun HeadUnitsContent(
    uiState: HeadUnitsUiState,
    onOpenHeadUnit: (String) -> Unit,
    onOpenSppTest: (() -> Unit)?,
    onRemoveHeadUnit: (String) -> Unit,
    onSetUp: () -> Unit,
    onAddClick: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Head units") },
                actions = {
                    onOpenSppTest?.let { TextButton(onClick = it) { Text("Bluetooth test") } }
                    AboutButton(onOpenSettings)
                },
            )
        },
        floatingActionButton = {
            FloatingActionButton(onClick = onAddClick) {
                Icon(painterResource(R.drawable.ic_add), contentDescription = "Add a car")
            }
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            if (uiState.headUnits.isEmpty()) {
                Column(
                    Modifier.fillMaxSize().padding(24.dp),
                    verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(
                        "No car added yet. The set-up adds it and pairs this phone with it.",
                        style = MaterialTheme.typography.bodyLarge,
                        textAlign = TextAlign.Center,
                    )
                    Button(onClick = onSetUp) { Text("Set up your car") }
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

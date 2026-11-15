// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairrequest

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.ui.headunits.AddHeadUnitFlow
import com.nbtapps.store.ui.headunits.AutostartSetupCard
import com.nbtapps.store.ui.headunits.EnableAutostartBox

/**
 * Pairs this phone with a car by a code another app handed over. It shows the code, asks which car it is for, or
 * for the car to be added, and on Pair connects and sends it. The code cannot be changed here: one the agent
 * refuses, or one that runs out, is replaced by a new one from the app that sent it.
 *
 * With the box that has the car start the agent with it ticked, the screen stays once the phone is paired and shows
 * how setting that up goes, until the user opens the app or closes it.
 *
 * [onPaired] is told once the agent has taken the code, where the box is cleared; [onOpenApp] when the user opens
 * the app from here, and [onClose] when the user is done with the screen.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PairRequestScreen(
    viewModel: PairRequestViewModel,
    onPaired: (String) -> Unit,
    onOpenApp: () -> Unit,
    onClose: () -> Unit,
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    var adding by rememberSaveable { mutableStateOf(false) }

    val step = uiState.step
    // The state is collected only while the screen is started, so the app is not opened from behind another.
    LaunchedEffect(step) {
        if (step is PairRequestStep.Paired && !uiState.enableAutostart) onPaired(step.name)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Pair this phone") },
                navigationIcon = {
                    IconButton(onClick = onClose) {
                        Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Close")
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
            val code = uiState.code
            val name = uiState.chosen?.name ?: "the car"
            when {
                !uiState.loaded || code == null -> CircularProgressIndicator()
                else -> when (step) {
                    PairRequestStep.Prompt -> Prompt(
                        code = code,
                        uiState = uiState,
                        onChoose = viewModel::choose,
                        onEnableAutostart = viewModel::setEnableAutostart,
                        onPair = viewModel::pair,
                        onAdd = { adding = true },
                    )
                    PairRequestStep.Connecting -> Working("Connecting to $name…")
                    PairRequestStep.Pairing -> Working("Pairing with $name…")
                    is PairRequestStep.Paired -> {
                        Text("Paired with ${step.name}.", style = MaterialTheme.typography.titleMedium)
                        if (uiState.enableAutostart) {
                            Setup(uiState, viewModel)
                            Button(onClick = onOpenApp) { Text("Open NBTApps") }
                        }
                    }
                    is PairRequestStep.AlreadyPaired -> {
                        Text("This phone is already paired with ${step.name}.", style = MaterialTheme.typography.titleMedium)
                        Text(
                            "There was nothing to send the code for.",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        if (uiState.enableAutostart) Setup(uiState, viewModel)
                        Button(onClick = onOpenApp) { Text("Open NBTApps") }
                    }
                    is PairRequestStep.NoLink -> {
                        Text("The code couldn't go to $name: the app has no link to it.", style = MaterialTheme.typography.titleMedium)
                        Text(step.reason, color = MaterialTheme.colorScheme.error)
                        if (!uiState.canRetry) Text(NEW_CODE)
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            if (uiState.canRetry) Button(onClick = viewModel::pair) { Text("Retry") }
                            OutlinedButton(onClick = onClose) { Text("Close") }
                        }
                    }
                    is PairRequestStep.Refused -> {
                        Text("The code didn't pair this phone.", style = MaterialTheme.typography.titleMedium)
                        Text(step.message, color = MaterialTheme.colorScheme.error)
                        Text(NEW_CODE)
                        OutlinedButton(onClick = onClose) { Text("Close") }
                    }
                    PairRequestStep.TimeUp -> {
                        Text("The code has run out.", style = MaterialTheme.typography.titleMedium)
                        Text(NEW_CODE)
                        OutlinedButton(onClick = onClose) { Text("Close") }
                    }
                }
            }
        }
    }

    AddHeadUnitFlow(
        show = adding,
        onDismiss = { adding = false },
        onAdded = { id ->
            adding = false
            viewModel.added(id)
        },
    )
}

private const val NEW_CODE = "Get a new code from the app that sent this one."

/** How setting the car up to start the agent with it goes, once the phone is paired. */
@Composable
private fun Setup(uiState: PairRequestUiState, viewModel: PairRequestViewModel) {
    uiState.setup?.let { setup ->
        AutostartSetupCard(
            step = setup,
            restartNeeded = uiState.restartNeeded,
            onRetry = viewModel::retrySetup,
            onRestart = viewModel::restartIdrive,
        )
    }
}

@Composable
private fun Prompt(
    code: String,
    uiState: PairRequestUiState,
    onChoose: (String) -> Unit,
    onEnableAutostart: (Boolean) -> Unit,
    onPair: () -> Unit,
    onAdd: () -> Unit,
) {
    Text(
        "Another app sent pairing code ${grouped(code)}",
        style = MaterialTheme.typography.titleMedium,
        modifier = Modifier.semantics { contentDescription = "Another app sent pairing code $code" },
    )
    uiState.remainingMs?.let { remaining ->
        Text(
            "It is good for another ${countdown(remaining)}.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    val units = uiState.headUnits
    when {
        units.isEmpty() -> {
            Text("Add your car to pair it.")
            // Adding the car goes straight on to pairing, so the box is here too.
            EnableAutostartBox(uiState.enableAutostart, onEnableAutostart)
            Button(onClick = onAdd) { Text("Add your car") }
            return
        }
        units.size == 1 -> Text("Pair this phone with ${units.single().name}?")
        else -> {
            Text("Pair this phone with which car?")
            Column(Modifier.selectableGroup()) {
                units.forEach { unit -> CarChoice(unit, selected = unit == uiState.chosen) { onChoose(unit.id) } }
            }
        }
    }
    EnableAutostartBox(uiState.enableAutostart, onEnableAutostart)
    Button(onClick = onPair, enabled = uiState.canPair) { Text("Pair") }
    TextButton(onClick = onAdd) { Text("A different car…") }
}

@Composable
private fun CarChoice(unit: HeadUnit, selected: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .selectable(selected = selected, onClick = onClick, role = Role.RadioButton)
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Text(unit.name, Modifier.padding(start = 8.dp))
    }
}

@Composable
private fun Working(text: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        Text(text)
    }
}

/** The code in two groups of three, as it is easier to read and compare: "123 456". */
internal fun grouped(code: String): String = code.chunked(3).joinToString(" ")

/** Whole seconds left, rounded up, as minutes and seconds: "1:05". */
internal fun countdown(remainingMs: Long): String {
    val seconds = (remainingMs + 999) / 1000
    return "${seconds / 60}:${(seconds % 60).toString().padStart(2, '0')}"
}

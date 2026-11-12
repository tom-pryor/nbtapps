// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.pairing

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.link.ProtocolConstants
import com.nbtapps.store.model.AgentText

/**
 * Pairs this phone with a head unit. It says where on the car's screen the code is shown and takes the code;
 * with no link to the head unit it offers to open one, since the code goes over it.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PairingScreen(
    onBack: () -> Unit,
    viewModel: PairingViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Pair this phone") },
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
            val name = uiState.headUnitName
            when {
                !uiState.loaded -> CircularProgressIndicator()
                name == null -> Text("Head unit not found.")
                else -> when (val step = uiState.step) {
                    PairingStep.Connecting -> Working("Connecting to the agent…")
                    is PairingStep.EnterCode -> CodeEntry(name, step.stale, uiState, viewModel::setCode, viewModel::pair)
                    PairingStep.Paired -> {
                        Text("This phone is paired with $name.", style = MaterialTheme.typography.titleMedium)
                        Text(
                            "It stays paired until its phones are forgotten on the car's screen, or $name is removed from this app.",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        Button(onClick = onBack) { Text("Done") }
                    }
                    is PairingStep.NoLink -> {
                        Text("The code from the car's screen goes to $name over the app's link to it, and there is none now.")
                        step.reason?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                        Button(onClick = viewModel::connect) { Text("Connect to the agent") }
                    }
                }
            }
        }
    }
}

@Composable
private fun CodeEntry(
    name: String,
    stale: Boolean,
    uiState: PairingUiState,
    onCode: (String) -> Unit,
    onPair: () -> Unit,
) {
    if (stale) {
        Text("$name no longer knows this phone: its paired phones were forgotten on the car. Pair it again to go on.")
    } else {
        Text("$name wants this phone paired before the app can do anything on it. It is done once.")
    }
    Text(AgentText.PAIR_ON_CAR)
    Text(
        "It shows ${ProtocolConstants.PAIRING_CODE_DIGITS} digits in two rows, good for " +
            "${ProtocolConstants.PAIRING_WINDOW_SECONDS / 60} minutes. Type them here, top row first. After " +
            "${ProtocolConstants.PAIRING_TRIES} wrong codes the car shows none for ${ProtocolConstants.PAIRING_PAUSE_SECONDS} seconds.",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    OutlinedTextField(
        value = uiState.code,
        onValueChange = onCode,
        label = { Text("Code from the car's screen") },
        singleLine = true,
        enabled = !uiState.working,
        isError = uiState.error != null,
        supportingText = uiState.error?.let { error -> { Text(error) } },
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = { onPair() }),
        modifier = Modifier.fillMaxWidth(),
    )
    if (uiState.working) {
        Working("Pairing…")
    } else {
        Button(onClick = onPair, enabled = uiState.canPair) { Text("Pair") }
    }
}

@Composable
private fun Working(text: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        Text(text)
    }
}

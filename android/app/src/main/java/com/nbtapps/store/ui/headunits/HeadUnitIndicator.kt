// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/**
 * For the actions of a top bar: the selected head unit by name, with a dot for the app's link to it. A tap opens
 * a menu to connect, disconnect, open the head unit's own screen ([onOpenHeadUnit]) or connect to another saved
 * one, and where the head unit wants the phone paired first, to pair it ([onPairHeadUnit]). With no head unit
 * saved it is a way to where one is added ([onAddHeadUnit]).
 */
@Composable
fun HeadUnitIndicator(
    onOpenHeadUnit: (String) -> Unit,
    onAddHeadUnit: () -> Unit,
    onPairHeadUnit: (String) -> Unit,
    viewModel: HeadUnitIndicatorViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    HeadUnitIndicatorContent(uiState, viewModel::connect, viewModel::disconnect, onOpenHeadUnit, onAddHeadUnit, onPairHeadUnit)
}

@Composable
private fun HeadUnitIndicatorContent(
    uiState: HeadUnitIndicatorUiState,
    onConnect: (String) -> Unit,
    onDisconnect: () -> Unit,
    onOpenHeadUnit: (String) -> Unit,
    onAddHeadUnit: () -> Unit,
    onPairHeadUnit: (String) -> Unit,
) {
    val unit = uiState.headUnit
    when {
        // Nothing until the saved head units are read: "Add head unit" must not flash up for a user who has one.
        !uiState.loaded -> Unit
        unit == null -> TextButton(onClick = onAddHeadUnit) { Text("Add head unit") }
        else -> Box {
            var open by remember { mutableStateOf(false) }
            TextButton(
                onClick = { open = true },
                // The dot says nothing to a screen reader: the state is read out with the name.
                modifier = Modifier.semantics { stateDescription = uiState.status.label },
                colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.onSurface),
            ) {
                StatusDot(uiState.status)
                Text(
                    unit.name,
                    Modifier.padding(start = 8.dp).widthIn(max = 140.dp),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                // A reason for a failed connect can be long: it wraps here, and does not widen the menu to the screen.
                Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp).widthIn(max = 240.dp)) {
                    Text(unit.name, style = MaterialTheme.typography.titleSmall)
                    Text(
                        uiState.detail,
                        style = MaterialTheme.typography.bodySmall,
                        color = if (uiState.status == IndicatorStatus.FAILED) {
                            MaterialTheme.colorScheme.error
                        } else {
                            MaterialTheme.colorScheme.onSurfaceVariant
                        },
                    )
                }
                HorizontalDivider()
                when (uiState.status) {
                    IndicatorStatus.CONNECTED ->
                        DropdownMenuItem(text = { Text("Disconnect") }, onClick = { open = false; onDisconnect() })
                    // The line above says it, and there is nothing to do but wait.
                    IndicatorStatus.CONNECTING -> Unit
                    // The link is up and held for the code, so it can be let go as a connected one can.
                    IndicatorStatus.NEEDS_PAIRING -> {
                        DropdownMenuItem(text = { Text("Pair this phone") }, onClick = { open = false; onPairHeadUnit(unit.id) })
                        DropdownMenuItem(text = { Text("Disconnect") }, onClick = { open = false; onDisconnect() })
                    }
                    IndicatorStatus.FAILED, IndicatorStatus.IDLE -> if (uiState.canConnect) {
                        DropdownMenuItem(
                            text = { Text(if (uiState.status == IndicatorStatus.FAILED) "Connect again" else "Connect") },
                            onClick = { open = false; onConnect(unit.id) },
                        )
                    }
                }
                DropdownMenuItem(text = { Text("Head unit details") }, onClick = { open = false; onOpenHeadUnit(unit.id) })
                if (uiState.others.isNotEmpty()) {
                    HorizontalDivider()
                    // The app has one link: a connect to another head unit ends this one, and a job over it with it.
                    val canSwitch = !uiState.busy && uiState.status != IndicatorStatus.CONNECTING
                    uiState.others.forEach { other ->
                        DropdownMenuItem(
                            text = { Text("Connect to ${other.name}", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                            onClick = { open = false; onConnect(other.id) },
                            enabled = canSwitch,
                        )
                    }
                    if (uiState.busy) {
                        Text(
                            "A job is running on ${unit.name}.",
                            Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}

/**
 * Filled for a link, an empty ring for none, the error colour for one that failed, a spinner while one is opened,
 * and a ring in the link's colour for one that waits for the phone to be paired: there, and not yet of use.
 */
@Composable
private fun StatusDot(status: IndicatorStatus) {
    // One size for all, so the name beside it does not move when the state changes.
    Box(Modifier.size(14.dp), contentAlignment = Alignment.Center) {
        when (status) {
            IndicatorStatus.CONNECTING -> CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp)
            IndicatorStatus.CONNECTED -> Box(Modifier.size(10.dp).background(MaterialTheme.colorScheme.primary, CircleShape))
            IndicatorStatus.NEEDS_PAIRING -> Box(Modifier.size(10.dp).border(2.5.dp, MaterialTheme.colorScheme.primary, CircleShape))
            IndicatorStatus.FAILED -> Box(Modifier.size(10.dp).background(MaterialTheme.colorScheme.error, CircleShape))
            IndicatorStatus.IDLE -> Box(Modifier.size(10.dp).border(1.5.dp, MaterialTheme.colorScheme.outline, CircleShape))
        }
    }
}

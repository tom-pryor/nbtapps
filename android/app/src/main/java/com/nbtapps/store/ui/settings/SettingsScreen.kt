// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.settings

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextFieldColors
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.settings.SettingRules
import com.nbtapps.store.data.settings.SettingsFailure
import com.nbtapps.store.model.SettingType

/**
 * The settings of one installed package, read from the head unit and changed here: a row for each option, the
 * advanced ones under a header that opens, a bar at the bottom while changes wait to be saved, and at the top, once
 * they are, a card that says when they take effect. Back with changes waiting asks first.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(onBack: () -> Unit, onUpdateAgent: () -> Unit, viewModel: SettingsViewModel = hiltViewModel()) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    var leaving by rememberSaveable { mutableStateOf(false) }
    val back = { if (uiState.count > 0) leaving = true else onBack() }
    BackHandler(enabled = uiState.count > 0) { leaving = true }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Settings of ${uiState.name}", maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = {
                    IconButton(onClick = back) {
                        Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back")
                    }
                },
            )
        },
        bottomBar = { if (uiState.edit != null && uiState.count > 0) SaveBar(uiState, viewModel::discard, viewModel::save) },
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            when {
                uiState.edit != null -> Settings(uiState, viewModel)
                !uiState.connected -> CannotRead(
                    "Connect to the head unit to see and change its settings. Its agent has to be one that can send them.",
                    button = "Go back",
                    onClick = onBack,
                )
                uiState.tooOld -> CannotRead(SettingsFailure.TooOld.message, button = "Update the agent", onClick = onUpdateAgent)
                else -> when (val load = uiState.load) {
                    is SettingsLoad.Failed -> CannotRead(load.failure.message, button = "Try again", onClick = viewModel::load, warning = load.failure == SettingsFailure.Busy)
                    else -> Reading()
                }
            }
        }
    }

    if (leaving) {
        AlertDialog(
            onDismissRequest = { leaving = false },
            title = { Text("Discard changes?") },
            text = { Text("${changes(uiState.count)} not been saved. If you leave now they are lost.") },
            confirmButton = {
                TextButton(
                    onClick = {
                        leaving = false
                        viewModel.discard()
                        onBack()
                    },
                ) { Text("Discard") }
            },
            dismissButton = { TextButton(onClick = { leaving = false }) { Text("Keep editing") } },
        )
    }
}

/** "1 change has" or "3 changes have", as the discard question begins. */
private fun changes(count: Int) = if (count == 1) "1 change has" else "$count changes have"

@Composable
private fun Reading() {
    Text(
        "The settings are read from the head unit each time this screen opens.",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        CircularProgressIndicator(Modifier.size(32.dp))
        Text("Reading the settings…")
    }
}

/** No settings to show: why, and the one thing to do about it. */
@Composable
private fun CannotRead(text: String, button: String, onClick: () -> Unit, warning: Boolean = false) {
    Text(
        "The settings are read from the head unit each time this screen opens.",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Text(text, color = if (warning) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.onSurface)
    OutlinedButton(onClick = onClick) { Text(button) }
}

@Composable
private fun Settings(uiState: SettingsUiState, viewModel: SettingsViewModel) {
    uiState.saved?.let { SavedCard(it, uiState.name) }
    when {
        uiState.linkLost -> Text(
            "The link to the head unit was lost. Your changes are kept here; connect again from the head unit's page to save them.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.error,
        )
        uiState.jobRunning -> Text(
            "An install is running on the head unit. Settings can be saved when it has finished; your changes are kept until then.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.tertiary,
        )
    }
    uiState.error?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error) }
    Text(
        "Read from the head unit when this screen opened. " +
            (uiState.takesEffect?.let { "Changes take effect $it." } ?: "Each setting says when a change takes effect."),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    val common = uiState.rows.filter { !it.advanced }
    val advanced = uiState.rows.filter { it.advanced }
    if (common.isNotEmpty()) Rows(common, viewModel)
    if (advanced.isNotEmpty()) {
        AdvancedHeader(advanced.size, uiState.advancedChanged, uiState.advancedOpen, viewModel::toggleAdvanced)
        if (uiState.advancedOpen) Rows(advanced, viewModel)
    }
    // Room to scroll the last row out from under the save bar.
    Spacer(Modifier.height(8.dp))
}

@Composable
private fun Rows(rows: List<OptionRow>, viewModel: SettingsViewModel) {
    Card(Modifier.fillMaxWidth()) {
        rows.forEachIndexed { i, row ->
            if (i > 0) HorizontalDivider(Modifier.padding(horizontal = 16.dp), color = MaterialTheme.colorScheme.outlineVariant)
            OptionRowView(row, onSet = { viewModel.set(row.key, it) }, onReset = { viewModel.reset(row.key) })
        }
    }
}

/** The header of the advanced options: closed at first, and saying so when it holds changes. */
@Composable
private fun AdvancedHeader(options: Int, changed: Int, open: Boolean, onToggle: () -> Unit) {
    Card(Modifier.fillMaxWidth().clickable(onClick = onToggle)) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Column(Modifier.weight(1f)) {
                Text("Advanced", style = MaterialTheme.typography.titleMedium)
                Text(
                    advancedLine(options, changed),
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (changed > 0) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Icon(
                painterResource(R.drawable.ic_expand_more),
                contentDescription = if (open) "Hide the advanced settings" else "Show the advanced settings",
                modifier = if (open) Modifier.rotate(180f) else Modifier,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/** What the advanced header says under its title: how many options, and how many of them have changes waiting. */
internal fun advancedLine(options: Int, changed: Int): String =
    (if (options == 1) "1 option" else "$options options") + if (changed > 0) " · $changed changed" else ""

/** The card at the top once changes are saved: when each of them takes effect. */
@Composable
private fun SavedCard(groups: List<SavedGroup>, name: String) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.primaryContainer,
            contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
        ),
    ) {
        Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Box(
                Modifier.size(28.dp).clip(CircleShape).background(MaterialTheme.colorScheme.primary),
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(R.drawable.ic_check), contentDescription = null, tint = MaterialTheme.colorScheme.onPrimary, modifier = Modifier.size(18.dp))
            }
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text("Saved", style = MaterialTheme.typography.titleMedium)
                groups.forEach { Text(savedLine(it, name), style = MaterialTheme.typography.bodyMedium) }
            }
        }
    }
}

/** The bar at the bottom while changes wait: how many, Discard, and Save. */
@Composable
private fun SaveBar(uiState: SettingsUiState, onDiscard: () -> Unit, onSave: () -> Unit) {
    Surface(color = MaterialTheme.colorScheme.surfaceContainer, tonalElevation = 3.dp) {
        Column {
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            Row(
                Modifier.navigationBarsPadding().padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Column(Modifier.weight(1f)) {
                    Text(if (uiState.count == 1) "1 change" else "${uiState.count} changes", style = MaterialTheme.typography.titleMedium)
                    Text(
                        saveBarLine(uiState),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                TextButton(onClick = onDiscard, enabled = !uiState.saving) { Text("Discard") }
                Button(onClick = onSave, enabled = uiState.canSave) { Text("Save") }
            }
        }
    }
}

/** What the save bar says under the count of changes: why Save is off, where it is. */
internal fun saveBarLine(uiState: SettingsUiState): String = when {
    uiState.saving -> "Saving…"
    uiState.linkLost -> "Not connected"
    uiState.jobRunning -> "Save when the install has finished"
    uiState.edit?.problems.orEmpty().isNotEmpty() -> "Correct the values in red to save"
    else -> "Not saved yet"
}

/** One option: label and description, its field, and the line under it, with Reset at its right. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun OptionRowView(row: OptionRow, onSet: (String) -> Unit, onReset: () -> Unit) {
    Column(Modifier.padding(horizontal = 16.dp, vertical = 14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Text(row.label, Modifier.weight(1f), style = MaterialTheme.typography.bodyLarge)
            if (row.type == SettingType.BOOL && !row.readOnly) {
                Switch(checked = row.on, onCheckedChange = { onSet(if (it) SettingRules.ON else SettingRules.OFF) })
            }
        }
        row.description?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        row.takesEffect?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        when {
            row.readOnly -> Text(READ_ONLY_NOTE, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.tertiary)
            row.type == SettingType.SELECT -> Choice(row, onSet)
            row.type == SettingType.INT || row.type == SettingType.TEXT -> Field(row, onSet)
        }
        if (row.byHand) {
            Text(
                if (row.type == SettingType.BOOL) {
                    "Set by hand on the head unit to \"${row.input}\", which isn't on or off. It is left as it is, unless you switch it or reset it."
                } else {
                    "Set by hand on the head unit to a value the app doesn't take. It is left as it is, unless you give another or reset it."
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.tertiary,
            )
        }
        row.error?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error) }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(
                row.meta,
                Modifier.weight(1f),
                style = MaterialTheme.typography.bodySmall,
                color = if (row.changed) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (row.canReset) TextButton(onClick = onReset) { Text("Reset") }
        }
    }
}

/** The outline of a field: red for a value that is not taken, amber for one set by hand, blue for a change. */
@Composable
private fun fieldColors(row: OptionRow): TextFieldColors {
    val accent = when {
        row.byHand -> MaterialTheme.colorScheme.tertiary
        row.changed -> MaterialTheme.colorScheme.primary
        else -> null
    }
    val text = if (row.muted) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurface
    return if (accent == null) {
        OutlinedTextFieldDefaults.colors(focusedTextColor = text, unfocusedTextColor = text)
    } else {
        OutlinedTextFieldDefaults.colors(
            focusedTextColor = text,
            unfocusedTextColor = text,
            focusedBorderColor = accent,
            unfocusedBorderColor = accent,
        )
    }
}

/** A number or a text. Not set, it is empty and shows the default, or what the manifest says then, muted. */
@Composable
private fun Field(row: OptionRow, onSet: (String) -> Unit) {
    OutlinedTextField(
        value = row.input,
        onValueChange = onSet,
        modifier = Modifier.fillMaxWidth(),
        singleLine = true,
        isError = row.error != null,
        textStyle = if (row.byHand) MaterialTheme.typography.bodyLarge.copy(fontFamily = FontFamily.Monospace) else MaterialTheme.typography.bodyLarge,
        placeholder = row.placeholder?.let { { Text(it) } },
        // Not beside a value set by hand, which is not a number of that unit.
        suffix = row.unit?.takeIf { !row.byHand }?.let { { Text(it) } },
        keyboardOptions = KeyboardOptions(keyboardType = if (row.type == SettingType.INT) KeyboardType.Number else KeyboardType.Text),
        colors = fieldColors(row),
    )
}

/** A choice of the option's values, as a dropdown. Not set, it shows the default or what the manifest says then. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun Choice(row: OptionRow, onSet: (String) -> Unit) {
    var open by rememberSaveable(row.key) { mutableStateOf(false) }
    val shown = row.choices.firstOrNull { it.value == row.input }?.label ?: row.input
    ExposedDropdownMenuBox(expanded = open, onExpandedChange = { open = it }) {
        OutlinedTextField(
            value = shown,
            onValueChange = {},
            readOnly = true,
            singleLine = true,
            isError = row.error != null,
            textStyle = if (row.byHand) MaterialTheme.typography.bodyLarge.copy(fontFamily = FontFamily.Monospace) else MaterialTheme.typography.bodyLarge,
            placeholder = row.placeholder?.let { { Text(it) } },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = open) },
            colors = fieldColors(row),
            modifier = Modifier.fillMaxWidth().menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable),
        )
        ExposedDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            row.choices.forEach { choice ->
                DropdownMenuItem(
                    text = { Text(choice.label) },
                    onClick = {
                        open = false
                        onSet(choice.value)
                    },
                )
            }
        }
    }
}

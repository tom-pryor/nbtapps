// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.ssh

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.provider.Settings
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.net.toUri
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.R
import com.nbtapps.store.data.ssh.ACCESS_LOCAL_NETWORK
import com.nbtapps.store.data.ssh.SshPortState
import com.nbtapps.store.ui.detail.Section

/** A command or a fingerprint: the fixed-width face. */
private val Mono = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 13.sp, lineHeight = 18.sp)

/**
 * "SSH from other apps": the port on the phone for other SSH clients, and their keys. Cards in order: the port, how to
 * connect and the head unit's key (while the port is on), and the keys. See [SshAccessViewModel].
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SshAccessScreen(onBack: () -> Unit, viewModel: SshAccessViewModel = hiltViewModel()) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val context = LocalContext.current
    val activity = LocalActivity.current
    // Android says whether it will still ask only right after a refusal, so that is when it is read.
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        viewModel.localNetworkAnswered(granted, askAgain = activity?.shouldShowRequestPermissionRationale(ACCESS_LOCAL_NETWORK) == true)
    }
    LaunchedEffect(uiState.askLocalNetwork) {
        if (uiState.askLocalNetwork) {
            viewModel.localNetworkAsked()
            permission.launch(ACCESS_LOCAL_NETWORK)
        }
    }
    val openSettings = {
        try {
            context.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, "package:${context.packageName}".toUri()))
        } catch (e: ActivityNotFoundException) {
            // A phone with no such page: the line says where to look.
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("SSH from other apps", maxLines = 1, overflow = TextOverflow.Ellipsis)
                        uiState.name?.let {
                            Text(it, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                        }
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onBack) { Icon(painterResource(R.drawable.ic_arrow_back), contentDescription = "Back") }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            if (uiState.paused) PausedBanner()
            Column(
                Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                PortCard(uiState, viewModel, openSettings)
                uiState.on?.let { on ->
                    Section("How to connect") {
                        uiState.commands.forEach { command ->
                            CopyRow(
                                label = command.label,
                                text = command.command,
                                description = command.copyDescription,
                            ) { copy(context, "SSH command", command.command) }
                        }
                        Text(IDLE_LINE, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Section("Head unit's key") {
                        Text(HOST_KEY_TEXT, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        CopyRow(label = null, text = on.hostKey, description = "Copy fingerprint") { copy(context, "Host key fingerprint", on.hostKey) }
                    }
                }
                KeysCard(uiState, viewModel)
            }
        }
    }

    uiState.adding?.let { form -> AddKeyDialog(form, viewModel) }
    uiState.removing?.let { key ->
        AlertDialog(
            onDismissRequest = viewModel::cancelRemove,
            title = { Text("Remove ${key.name}?") },
            text = { Text("Its key will no longer be let in. Open connections are closed.") },
            confirmButton = { TextButton(onClick = viewModel::confirmRemove) { Text("Remove") } },
            dismissButton = { TextButton(onClick = viewModel::cancelRemove) { Text("Cancel") } },
        )
    }
}

private fun copy(context: Context, label: String, text: String) {
    context.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText(label, text))
}

@Composable
private fun PausedBanner() {
    Row(
        Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.tertiaryContainer).padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(painterResource(R.drawable.ic_warning), contentDescription = null, tint = MaterialTheme.colorScheme.onTertiaryContainer)
        Text("Paused while a job runs on the head unit", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onTertiaryContainer)
    }
}

@Composable
private fun PortCard(uiState: SshAccessUiState, viewModel: SshAccessViewModel, onOpenSettings: () -> Unit) {
    Section("Port") {
        ListItem(
            headlineContent = { Text("Let other apps connect") },
            supportingContent = {
                if (uiState.own is SshPortState.Checking) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp)
                        Text(uiState.switchLine)
                    }
                } else {
                    Text(uiState.switchLine)
                }
            },
            trailingContent = { Switch(checked = uiState.switchOn, onCheckedChange = viewModel::setPort, enabled = uiState.switchEnabled) },
        )
        uiState.offReason?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        uiState.failure?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error) }
        ListItem(
            headlineContent = { Text("Allow other devices") },
            supportingContent = { Text(uiState.otherDevicesLine) },
            trailingContent = {
                Switch(
                    checked = uiState.on?.otherDevices == true,
                    onCheckedChange = viewModel::setOtherDevices,
                    enabled = uiState.otherDevicesEnabled,
                )
            },
        )
        if (uiState.on?.otherDevices == true) {
            Text(OTHER_DEVICES_WARNING, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.tertiary)
        }
        uiState.on?.otherDevicesFailure?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error) }
        uiState.localNetworkLine?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error) }
        if (uiState.localNetworkRefusal == LocalNetworkRefusal.FOR_GOOD) {
            TextButton(onClick = onOpenSettings) { Text("Open settings") }
        }
    }
}

/** A command or a fingerprint, with its label above it where it has one, and a button that copies it. */
@Composable
private fun CopyRow(label: String?, text: String, description: String, onCopy: () -> Unit) {
    ListItem(
        overlineContent = label?.let { { Text(it) } },
        headlineContent = { Text(text, style = Mono) },
        trailingContent = {
            IconButton(onClick = onCopy) { Icon(painterResource(R.drawable.ic_content_copy), contentDescription = description) }
        },
    )
}

@Composable
private fun KeysCard(uiState: SshAccessUiState, viewModel: SshAccessViewModel) {
    Section("Keys") {
        if (uiState.keys.isEmpty()) {
            Text(NO_KEYS_TEXT, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        uiState.keys.forEach { key ->
            ListItem(
                headlineContent = { Text(key.name) },
                supportingContent = { Text(shortFingerprint(key.fingerprint), style = Mono.copy(fontSize = 12.sp, lineHeight = 16.sp)) },
                trailingContent = { TextButton(onClick = { viewModel.askRemove(key) }) { Text("Remove") } },
            )
        }
        Button(onClick = viewModel::openAdd, enabled = uiState.canAdd) { Text("Add key") }
        if (!uiState.canAdd) {
            Text(KEYS_FULL, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/** "Add a key": a name and the public key line, checked when Add is pressed; a refusal keeps it open. */
@Composable
private fun AddKeyDialog(form: AddKeyForm, viewModel: SshAccessViewModel) {
    AlertDialog(
        onDismissRequest = viewModel::cancelAdd,
        title = { Text("Add a key") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = form.name,
                    onValueChange = { viewModel.editAdd(form.copy(name = it)) },
                    label = { Text("Name") },
                    isError = form.nameError != null,
                    supportingText = form.nameError?.let { { Text(it) } },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = form.line,
                    onValueChange = { viewModel.editAdd(form.copy(line = it)) },
                    label = { Text("Public key") },
                    isError = form.keyError != null,
                    supportingText = form.keyError?.let { { Text(it) } },
                    textStyle = Mono,
                    minLines = 3,
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.None,
                        autoCorrectEnabled = false,
                        keyboardType = KeyboardType.PasswordVisible,
                    ),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        },
        confirmButton = { TextButton(onClick = viewModel::confirmAdd) { Text("Add") } },
        dismissButton = { TextButton(onClick = viewModel::cancelAdd) { Text("Cancel") } },
    )
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.model.PairedDevice

/**
 * Asks for a head unit's name and how to reach it: the car among the devices paired with this phone, a network
 * address, or both. The address is asked for only where [takesAddress]: where this build has a TCP link at all.
 * [bluetooth] is the phone's Bluetooth as it stands; [onAllowBluetooth] asks the user for the Nearby devices
 * permission, without which the paired devices cannot be listed.
 */
@Composable
fun AddHeadUnitDialog(
    bluetooth: BluetoothChoice,
    takesAddress: Boolean,
    onAllowBluetooth: () -> Unit,
    onDismiss: () -> Unit,
    onConfirm: (HeadUnitForm) -> Unit,
) {
    var entered by remember { mutableStateOf(HeadUnitForm(takesAddress = takesAddress)) }
    var showErrors by remember { mutableStateOf(false) }
    // A device that is no longer paired, or cannot be seen to be, is not a choice any more.
    val form = entered.forDevices(bluetooth.devices)

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Add head unit") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                FormField("Name", form.name, form.nameError.takeIf { showErrors }) { entered = form.copy(name = it) }

                Text("Bluetooth", style = MaterialTheme.typography.labelLarge)
                BluetoothPicker(bluetooth, form.bluetooth, onAllowBluetooth) { device -> entered = form.withDevice(device) }

                if (form.takesAddress) {
                    Text("Network address", style = MaterialTheme.typography.labelLarge)
                    Text(
                        "For the test bench only: a car is reached over Bluetooth. Not needed when the car is picked above.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    FormField("Host or IP address", form.host, form.hostError.takeIf { showErrors }, KeyboardType.Uri) {
                        entered = form.copy(host = it)
                    }
                    FormField("Port", form.port, form.portError.takeIf { showErrors }, KeyboardType.Number) {
                        entered = form.copy(port = it)
                    }
                }
                form.linkError?.takeIf { showErrors }?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            }
        },
        confirmButton = {
            TextButton(onClick = { showErrors = true; onConfirm(form) }) { Text("Add") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}

/** The form with its device dropped if [devices] no longer has it. */
internal fun HeadUnitForm.forDevices(devices: List<PairedDevice>): HeadUnitForm =
    if (bluetooth == null || bluetooth in devices) this else copy(bluetooth = null)

/** The form with [device] picked, or none. A name not yet entered becomes the device's: it is the car's own. */
internal fun HeadUnitForm.withDevice(device: PairedDevice?): HeadUnitForm =
    copy(bluetooth = device, name = if (device != null && name.isBlank()) device.name else name)

@Composable
private fun BluetoothPicker(
    bluetooth: BluetoothChoice,
    selected: PairedDevice?,
    onAllow: () -> Unit,
    onSelect: (PairedDevice?) -> Unit,
) {
    when (bluetooth.access) {
        BluetoothAccess.MISSING -> Text("This phone has no Bluetooth.")
        BluetoothAccess.NOT_ALLOWED -> {
            Text("To list the devices paired with this phone, the app needs the Nearby devices permission.")
            OutlinedButton(onClick = onAllow) { Text("Allow") }
        }
        BluetoothAccess.OFF -> Text("Bluetooth is off. Switch it on to pick the car.")
        BluetoothAccess.ON -> if (bluetooth.devices.isEmpty()) {
            Text("No paired devices. Pair the phone with the car in the phone's Bluetooth settings first.")
        } else {
            Column(Modifier.selectableGroup()) {
                Choice("Not over Bluetooth", selected = selected == null) { onSelect(null) }
                bluetooth.devices.forEach { device ->
                    Choice(device.name, selected = device == selected) { onSelect(device) }
                }
            }
        }
    }
}

@Composable
private fun Choice(label: String, selected: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .selectable(selected = selected, onClick = onClick, role = Role.RadioButton)
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null)
        Text(label, Modifier.padding(start = 8.dp))
    }
}

@Composable
private fun FormField(
    label: String,
    value: String,
    error: String?,
    keyboardType: KeyboardType = KeyboardType.Text,
    onValueChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        label = { Text(label) },
        isError = error != null,
        supportingText = error?.let { { Text(it) } },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = keyboardType),
        modifier = Modifier.fillMaxWidth(),
    )
}

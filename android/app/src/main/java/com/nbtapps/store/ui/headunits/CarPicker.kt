// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import com.nbtapps.store.R
import com.nbtapps.store.model.PairedDevice

/** What a device's row says when it has a hands-free or audio connection to the phone now. */
const val CONNECTED_NOW = "Connected now"

/** What a device's row says otherwise. */
const val NOT_CONNECTED = "Not connected"

/**
 * The devices paired with this phone, for the user to pick the car from, as [state] has them ([carPickerState]): a
 * card of the cars, each with whether it is connected now and a radio button; under it, where there are any, the
 * other paired devices behind a row that opens them; and last a line that leads to Bluetooth settings for a car
 * that is not listed. In a state that blocks the list it says why instead, with the one button that helps
 * ([carPickerMessage]).
 *
 * [selected] is the address of the device picked, or null; [onSelect] is told the device the user taps. [onAction]
 * is told a button's action, which the caller carries out ([CarPickerFlow] does all of it). [compact] is the
 * tighter form, for a screen that has a lot else to show: the same list and states, with at most one line of
 * explanation.
 */
@Composable
fun CarPicker(
    state: CarPickerState,
    selected: String?,
    onSelect: (PairedDevice) -> Unit,
    onAction: (CarPickerAction) -> Unit,
    modifier: Modifier = Modifier,
    compact: Boolean = false,
) {
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(if (compact) 4.dp else 12.dp)) {
        val message = carPickerMessage(state, compact)
        if (message != null || state !is CarPickerState.Devices) {
            message?.let { Message(it, compact, onAction) }
            return@Column
        }
        val picked = state.find(selected)?.device
        DeviceCard(state.cars, picked, compact, onSelect)
        if (state.others.isNotEmpty()) {
            // Open to begin with where the device picked is one of them, so the pick is never hidden.
            var open by rememberSaveable { mutableStateOf(state.others.any { it.device == picked }) }
            ExpandRow("Other paired devices", otherDevicesLine(state.others.size), open, onToggle = { open = !open })
            if (open) DeviceCard(state.others, picked, compact, onSelect)
        }
        CarNotListed(onClick = { onAction(CarPickerAction.BLUETOOTH_SETTINGS) })
    }
}

@Composable
private fun DeviceCard(choices: List<CarChoice>, picked: PairedDevice?, compact: Boolean, onSelect: (PairedDevice) -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.selectableGroup()) {
            choices.forEachIndexed { index, choice ->
                if (index > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                DeviceRow(choice, selected = choice.device == picked, compact) { onSelect(choice.device) }
            }
        }
    }
}

@Composable
private fun DeviceRow(choice: CarChoice, selected: Boolean, compact: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = if (compact) 48.dp else 64.dp)
            .selectable(selected = selected, onClick = onClick, role = Role.RadioButton)
            .padding(horizontal = 16.dp, vertical = if (compact) 4.dp else 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            painterResource(if (choice.device.car) R.drawable.ic_head_unit else R.drawable.ic_bluetooth),
            contentDescription = null,
            modifier = Modifier.size(if (compact) 20.dp else 24.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Column(Modifier.weight(1f).padding(start = 16.dp)) {
            Text(choice.device.name, style = MaterialTheme.typography.bodyLarge)
            Text(
                if (choice.connected) CONNECTED_NOW else NOT_CONNECTED,
                style = MaterialTheme.typography.bodySmall,
                color = if (choice.connected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        RadioButton(selected = selected, onClick = null)
    }
}

@Composable
private fun Message(message: CarPickerMessage, compact: Boolean, onAction: (CarPickerAction) -> Unit) {
    Text(message.text, style = if (compact) MaterialTheme.typography.bodyMedium else MaterialTheme.typography.bodyLarge)
    message.detail?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant) }
    message.action?.let { action -> Button(onClick = { onAction(action) }) { Text(action.label) } }
}

/** [CAR_NOT_LISTED], its link drawn as one. The whole line is the touch target, so it is big enough to hit. */
@Composable
private fun CarNotListed(onClick: () -> Unit) {
    val link = SpanStyle(color = MaterialTheme.colorScheme.primary, textDecoration = TextDecoration.Underline)
    val text = buildAnnotatedString {
        append(CAR_NOT_LISTED)
        val start = CAR_NOT_LISTED.indexOf(CAR_NOT_LISTED_LINK)
        if (start >= 0) addStyle(link, start, start + CAR_NOT_LISTED_LINK.length)
    }
    Box(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .clickable(role = Role.Button, onClickLabel = CarPickerAction.BLUETOOTH_SETTINGS.label, onClick = onClick),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

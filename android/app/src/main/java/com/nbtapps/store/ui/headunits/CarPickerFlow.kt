// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.ActivityNotFoundException
import android.content.Intent
import android.provider.Settings
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.core.net.toUri
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.model.PairedDevice
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * The car picker with all it needs, wherever a screen offers it: the phone's Bluetooth, read again each time the
 * screen comes back (pairing a device, switching Bluetooth on and granting the permission happen outside the app),
 * the ask for the Nearby devices permission, and the pages of Android's settings its buttons open. See [CarPicker]
 * for what it shows; [compact] is its tighter form.
 *
 * The caller holds the choice: [selected] is the address of the device picked, or null, and [onPick] is told the
 * device the user picks. With [pickDefault], while nothing is picked, [onPick] is also told the car [defaultCar]
 * finds, as soon as there is one: the user can still pick another.
 */
@Composable
fun CarPickerFlow(
    selected: String?,
    onPick: (PairedDevice) -> Unit,
    modifier: Modifier = Modifier,
    compact: Boolean = false,
    pickDefault: Boolean = true,
    viewModel: CarPickerViewModel = hiltViewModel(),
) {
    val bluetooth by viewModel.bluetooth.collectAsStateWithLifecycle()
    val connected by viewModel.connected.collectAsStateWithLifecycle()
    // Android says whether it will still ask only right after a refusal, so that is when it is read.
    var refusedForGood by rememberSaveable { mutableStateOf(false) }
    val activity = LocalActivity.current
    val context = LocalContext.current

    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        refusedForGood = !granted && activity?.shouldShowRequestPermissionRationale(Manifest.permission.BLUETOOTH_CONNECT) == false
        viewModel.refresh()
    }
    val page = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { viewModel.refresh() }
    LifecycleResumeEffect(Unit) {
        viewModel.refresh()
        onPauseOrDispose {}
    }
    // Granted after all, in Android's settings: a later refusal is read afresh.
    LaunchedEffect(bluetooth.access) {
        if (bluetooth.access != BluetoothAccess.NOT_ALLOWED) refusedForGood = false
    }

    val state = carPickerState(bluetooth.access, bluetooth.devices, connected, refusedForGood)
    if (pickDefault && selected == null) {
        val default = defaultCar(bluetooth.devices, connected)
        LaunchedEffect(default) { default?.let(onPick) }
    }

    CarPicker(
        state = state,
        selected = selected,
        onSelect = onPick,
        onAction = { action ->
            when (action) {
                CarPickerAction.ALLOW -> permission.launch(Manifest.permission.BLUETOOTH_CONNECT)
                CarPickerAction.SWITCH_ON -> page.open(switchOnIntent())
                CarPickerAction.BLUETOOTH_SETTINGS -> page.open(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                CarPickerAction.APP_SETTINGS ->
                    page.open(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, "package:${context.packageName}".toUri()))
            }
        },
        modifier = modifier,
        compact = compact,
    )
}

/** Android's ask to switch Bluetooth on. Offered only with the Nearby devices permission granted, which it needs. */
private fun switchOnIntent() = Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)

/** Opens [intent]. A phone that has no such page leaves the picker as it is, with its words saying what to look for. */
private fun ActivityResultLauncher<Intent>.open(intent: Intent) {
    try {
        launch(intent)
    } catch (e: ActivityNotFoundException) {
        // Nothing more to do: the screen is read again when the user comes back anyway.
    }
}

/**
 * The phone's Bluetooth as the car picker needs it: whether it can be used, the paired devices, and which of them
 * are connected now. [refresh] reads it all again.
 */
@HiltViewModel
class CarPickerViewModel @Inject constructor(private val phone: PhoneBluetooth) : ViewModel() {

    private val read = MutableStateFlow(readBluetooth())

    /** The permission, Bluetooth's state and the paired devices, as last read. */
    val bluetooth: StateFlow<BluetoothChoice> = read.asStateFlow()

    /** The addresses of the paired devices connected now: see [PhoneBluetooth.connected]. */
    val connected: StateFlow<Set<String>> = phone.connected

    init {
        phone.refreshConnected()
    }

    /** Reads it all again: after the permission prompt or a page of Android's settings, and on return to the screen. */
    fun refresh() {
        read.value = readBluetooth()
        phone.refreshConnected()
    }

    private fun readBluetooth() = phone.access().let { access ->
        BluetoothChoice(access, if (access == BluetoothAccess.ON) phone.pairedDevices() else emptyList())
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.headunits

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.PairingKeys
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.data.link.LinkConnector
import com.nbtapps.store.data.link.reachesTcp
import com.nbtapps.store.data.removeWithKey
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.PairedDevice
import dagger.hilt.android.lifecycle.HiltViewModel
import java.util.UUID
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** What the phone's Bluetooth offers to pick a car from, as last read. */
data class BluetoothChoice(
    val access: BluetoothAccess = BluetoothAccess.OFF,
    /** The devices paired with this phone. Empty unless [access] is [BluetoothAccess.ON]. */
    val devices: List<PairedDevice> = emptyList(),
)

data class HeadUnitsUiState(
    val headUnits: List<HeadUnit> = emptyList(),
    val bluetooth: BluetoothChoice = BluetoothChoice(),
    /** Whether this build can reach a head unit at a network address, so whether the add dialog asks for one. */
    val takesAddress: Boolean = false,
)

@HiltViewModel
class HeadUnitsViewModel @Inject constructor(
    private val repository: HeadUnitRepository,
    private val phoneBluetooth: PhoneBluetooth,
    connector: LinkConnector,
    private val keys: PairingKeys,
) : ViewModel() {

    private val bluetooth = MutableStateFlow(readBluetooth())
    private val takesAddress = connector.reachesTcp

    val uiState: StateFlow<HeadUnitsUiState> =
        combine(repository.headUnits, bluetooth) { units, bluetooth -> HeadUnitsUiState(units, bluetooth, takesAddress) }
            .stateIn(
                viewModelScope,
                SharingStarted.WhileSubscribed(5_000),
                HeadUnitsUiState(bluetooth = bluetooth.value, takesAddress = takesAddress),
            )

    /**
     * Reads the permission and the paired devices again: after the permission prompt, and on return to the
     * screen, since pairing a device or switching Bluetooth on happens outside the app.
     */
    fun refreshBluetooth() {
        bluetooth.value = readBluetooth()
    }

    /**
     * Adds the head unit described by [form]. Returns false and does nothing if the form is invalid. An address in
     * it is dropped where this build has no TCP link.
     */
    fun addHeadUnit(form: HeadUnitForm): Boolean {
        val checked = form.copy(takesAddress = form.takesAddress && takesAddress)
        if (!checked.isValid) return false
        viewModelScope.launch { repository.add(checked.toHeadUnit(UUID.randomUUID().toString())) }
        return true
    }

    /** Removes the head unit, and the key it issued this phone when the two were paired. */
    fun removeHeadUnit(id: String) {
        viewModelScope.launch { repository.removeWithKey(id, keys) }
    }

    private fun readBluetooth() = BluetoothChoice(phoneBluetooth.access(), phoneBluetooth.pairedDevices())
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.spptest

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.data.SppEchoClient
import com.nbtapps.store.model.EchoReport
import com.nbtapps.store.model.EchoSize
import com.nbtapps.store.model.PairedDevice
import com.nbtapps.store.model.SppEcho
import dagger.hilt.android.lifecycle.HiltViewModel
import java.util.UUID
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

sealed interface EchoRunState {
    data object Idle : EchoRunState
    data class Running(val received: Int, val total: Int) : EchoRunState
    data class Done(val report: EchoReport) : EchoRunState
    data class Failed(val reason: String) : EchoRunState
}

data class SppTestUiState(
    val permissionGranted: Boolean = false,
    val devices: List<PairedDevice> = emptyList(),
    val selectedAddress: String? = null,
    val uuid: String = SppEcho.DEFAULT_SERVICE_UUID,
    val size: EchoSize = EchoSize.SMALL,
    val run: EchoRunState = EchoRunState.Idle,
) {
    val uuidError: String?
        get() = if (SppEcho.isValidUuid(uuid)) null else "Enter a UUID like ${SppEcho.DEFAULT_SERVICE_UUID}"

    val canRun: Boolean
        get() = permissionGranted && selectedAddress != null && uuidError == null && run !is EchoRunState.Running
}

@HiltViewModel
class SppTestViewModel @Inject constructor(
    private val client: SppEchoClient,
    private val bluetooth: PhoneBluetooth,
) : ViewModel() {

    private val _uiState = MutableStateFlow(SppTestUiState())
    val uiState: StateFlow<SppTestUiState> = _uiState.asStateFlow()

    init {
        refreshDevices()
    }

    /** Re-reads the permission and the paired devices: call after the permission prompt and on return to the screen. */
    fun refreshDevices() {
        val devices = bluetooth.pairedDevices()
        val allowed = bluetooth.access() != BluetoothAccess.NOT_ALLOWED
        _uiState.update { state ->
            state.copy(
                permissionGranted = allowed,
                devices = devices,
                selectedAddress = state.selectedAddress?.takeIf { address -> devices.any { it.address == address } },
            )
        }
    }

    fun selectDevice(address: String) = _uiState.update { it.copy(selectedAddress = address) }

    fun setUuid(uuid: String) = _uiState.update { it.copy(uuid = uuid) }

    fun setSize(size: EchoSize) = _uiState.update { it.copy(size = size) }

    fun runEcho() {
        val state = _uiState.value
        val address = state.selectedAddress
        if (!state.canRun || address == null) return
        val total = state.size.bytes
        _uiState.update { it.copy(run = EchoRunState.Running(received = 0, total = total)) }
        viewModelScope.launch {
            val result = client.runEcho(address, UUID.fromString(state.uuid.trim()), total) { received ->
                _uiState.update { current ->
                    // Progress can arrive after the run has ended; only a running test shows it.
                    if (current.run is EchoRunState.Running) current.copy(run = EchoRunState.Running(received, total)) else current
                }
            }
            val outcome = result.fold(
                onSuccess = { EchoRunState.Done(it) },
                onFailure = { EchoRunState.Failed(it.message ?: it.javaClass.simpleName) },
            )
            _uiState.update { it.copy(run = outcome) }
        }
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.navigation

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.HeadUnitRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/**
 * Which bottom-bar screen the app opens on: the catalogue, or with no head unit saved, the head units list,
 * where one is added. It is decided once when the app starts and kept in the saved state: a head unit added or
 * removed later does not move the start from under the screens stacked on it, and a stack restored after the
 * process was stopped finds the start it was built on.
 */
@HiltViewModel
class StartViewModel @Inject constructor(
    headUnits: HeadUnitRepository,
    private val savedStateHandle: SavedStateHandle,
) : ViewModel() {

    /** Whether the app opens on the head units list. Null until the saved head units have been read. */
    val startOnHeadUnits: StateFlow<Boolean?> = savedStateHandle.getStateFlow<Boolean?>(START_ON_HEAD_UNITS, null)

    init {
        if (startOnHeadUnits.value == null) {
            viewModelScope.launch { savedStateHandle[START_ON_HEAD_UNITS] = headUnits.headUnits.first().isEmpty() }
        }
    }

    private companion object {
        const val START_ON_HEAD_UNITS = "startOnHeadUnits"
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.navigation

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.nbtapps.store.data.HeadUnitRepository
import com.nbtapps.store.data.SetupRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/**
 * Whether the app opens the set-up for a new user over the catalogue, which is always the screen it opens on: so
 * it does where no head unit is saved and the user has not skipped the set-up. It is decided once when the app
 * starts, and kept in the saved state with whether the set-up has been opened since: a rotation, or a stack
 * restored after the process was stopped, does not open it a second time.
 */
@HiltViewModel
class StartViewModel @Inject constructor(
    headUnits: HeadUnitRepository,
    setup: SetupRepository,
    private val savedStateHandle: SavedStateHandle,
) : ViewModel() {

    /**
     * Whether the set-up is to be opened over the catalogue: true until it has been ([setupOpened]). Null until the
     * saved head units and the user's choice have been read.
     */
    val openSetup: StateFlow<Boolean?> = savedStateHandle.getStateFlow<Boolean?>(OPEN_SETUP, null)

    init {
        if (openSetup.value == null) {
            viewModelScope.launch {
                val due = headUnits.headUnits.first().isEmpty() && !setup.skipped.first()
                savedStateHandle[OPEN_SETUP] = due
            }
        }
    }

    /** The set-up was opened over the catalogue: it is not opened again for this start of the app. */
    fun setupOpened() {
        savedStateHandle[OPEN_SETUP] = false
    }

    private companion object {
        const val OPEN_SETUP = "openSetup"
    }
}

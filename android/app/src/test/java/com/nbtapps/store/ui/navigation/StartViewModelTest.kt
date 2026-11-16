// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.navigation

import androidx.lifecycle.SavedStateHandle
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
import com.nbtapps.store.data.InMemorySetupRepository
import com.nbtapps.store.model.HeadUnit
import com.nbtapps.store.model.TcpEndpoint
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class StartViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    private val repository = InMemoryHeadUnitRepository()
    private val setup = InMemorySetupRepository()
    private val car = HeadUnit("car", "Car", listOf(TcpEndpoint("192.168.0.1", 7144)))

    private fun viewModel(state: SavedStateHandle = SavedStateHandle()) = StartViewModel(repository, setup, state)

    @Test
    fun `with no head unit saved the set-up is opened over the catalogue`() = runTest {
        assertEquals(true, viewModel().openSetup.value)
    }

    @Test
    fun `with a head unit saved it is not`() = runTest {
        repository.add(car)
        assertEquals(false, viewModel().openSetup.value)
    }

    @Test
    fun `once skipped it is not, with no head unit saved either`() = runTest {
        setup.setSkipped(true)
        assertEquals(false, viewModel().openSetup.value)
    }

    @Test
    fun `it is opened once, and neither a rotation nor a restored process opens it again`() = runTest {
        val savedState = SavedStateHandle()
        val viewModel = viewModel(savedState)
        assertEquals(true, viewModel.openSetup.value)

        viewModel.setupOpened()

        assertEquals(false, viewModel.openSetup.value)
        // The process was stopped and the app restored from its saved state.
        val restored = viewModel(SavedStateHandle(savedState.keys().associateWith { savedState.get<Any?>(it) }))
        assertEquals(false, restored.openSetup.value)
    }

    @Test
    fun `it is decided once, and a head unit added or removed later does not change it`() = runTest {
        val savedState = SavedStateHandle()
        val viewModel = viewModel(savedState)
        repository.add(car)
        assertEquals(true, viewModel.openSetup.value)
        assertEquals(true, viewModel(savedState).openSetup.value)
    }
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.navigation

import androidx.lifecycle.SavedStateHandle
import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.InMemoryHeadUnitRepository
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
    private val car = HeadUnit("car", "Car", listOf(TcpEndpoint("192.168.0.1", 7144)))

    @Test
    fun `with no head unit saved the app opens on the head units list`() = runTest {
        assertEquals(true, StartViewModel(repository, SavedStateHandle()).startOnHeadUnits.value)
    }

    @Test
    fun `with a head unit saved the app opens on the catalogue`() = runTest {
        repository.add(car)
        assertEquals(false, StartViewModel(repository, SavedStateHandle()).startOnHeadUnits.value)
    }

    @Test
    fun `the start is decided once, and a head unit added or removed later does not move it`() = runTest {
        val savedState = SavedStateHandle()
        val viewModel = StartViewModel(repository, savedState)
        repository.add(car)
        assertEquals(true, viewModel.startOnHeadUnits.value)

        // The process was stopped and the app restored: the start is the one its stack was built on.
        assertEquals(true, StartViewModel(repository, savedState).startOnHeadUnits.value)
    }
}

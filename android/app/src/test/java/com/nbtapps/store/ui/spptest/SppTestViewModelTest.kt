// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.ui.spptest

import com.nbtapps.store.MainDispatcherRule
import com.nbtapps.store.data.BluetoothAccess
import com.nbtapps.store.data.FakePhoneBluetooth
import com.nbtapps.store.data.SppEchoClient
import com.nbtapps.store.model.EchoReport
import com.nbtapps.store.model.EchoSize
import com.nbtapps.store.model.SppEcho
import java.io.IOException
import java.util.UUID
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class SppTestViewModelTest {

    @get:Rule
    val mainDispatcherRule = MainDispatcherRule()

    private class FakeSppEchoClient : SppEchoClient {
        var result: Result<EchoReport> = Result.success(report(1024))
        var progress: List<Int> = emptyList()
        /** When set, the run waits here after reporting its progress. */
        var gate: CompletableDeferred<Unit>? = null
        val calls = mutableListOf<Triple<String, UUID, Int>>()

        override suspend fun runEcho(
            address: String,
            serviceUuid: UUID,
            totalBytes: Int,
            onProgress: (received: Int) -> Unit,
        ): Result<EchoReport> {
            calls += Triple(address, serviceUuid, totalBytes)
            progress.forEach(onProgress)
            gate?.await()
            return result
        }
    }

    private val client = FakeSppEchoClient()
    private val bluetooth = FakePhoneBluetooth()
    // Lazy so the ViewModel is created after the rule has installed the test Main dispatcher.
    private val viewModel by lazy { SppTestViewModel(client, bluetooth) }

    @Test
    fun `without the permission nothing is listed and the test can't run`() {
        bluetooth.access = BluetoothAccess.NOT_ALLOWED

        val state = viewModel.uiState.value

        assertFalse(state.permissionGranted)
        assertTrue(state.devices.isEmpty())
        assertFalse(state.canRun)
    }

    @Test
    fun `granting the permission lists the paired devices`() {
        bluetooth.access = BluetoothAccess.NOT_ALLOWED
        viewModel.uiState.value
        bluetooth.access = BluetoothAccess.ON

        viewModel.refreshDevices()

        val state = viewModel.uiState.value
        assertTrue(state.permissionGranted)
        assertEquals(listOf("Test Car", "Headphones"), state.devices.map { it.name })
        assertFalse("no device chosen yet", state.canRun)
    }

    @Test
    fun `with Bluetooth off the permission is not what is missing`() {
        bluetooth.access = BluetoothAccess.OFF

        val state = viewModel.uiState.value

        assertTrue(state.permissionGranted)
        assertTrue(state.devices.isEmpty())
    }

    @Test
    fun `the default service is the head unit echo tool's`() {
        assertEquals(SppEcho.DEFAULT_SERVICE_UUID, viewModel.uiState.value.uuid)
        assertNull(viewModel.uiState.value.uuidError)
    }

    @Test
    fun `an invalid UUID blocks the run`() {
        viewModel.selectDevice("00:11:22:33:44:55")
        viewModel.setUuid("1101")

        viewModel.runEcho()

        assertNotNull(viewModel.uiState.value.uuidError)
        assertFalse(viewModel.uiState.value.canRun)
        assertTrue(client.calls.isEmpty())
    }

    @Test
    fun `a run reports progress, then the result`() = runTest {
        client.progress = listOf(16, 4_096)
        client.gate = CompletableDeferred()
        client.result = Result.success(report(EchoSize.MEDIUM.bytes))
        viewModel.selectDevice("00:11:22:33:44:55")
        viewModel.setSize(EchoSize.MEDIUM)

        viewModel.runEcho()

        assertEquals(EchoRunState.Running(received = 4_096, total = EchoSize.MEDIUM.bytes), viewModel.uiState.value.run)
        assertFalse("no second run while one is going", viewModel.uiState.value.canRun)
        assertEquals(
            listOf(Triple("00:11:22:33:44:55", UUID.fromString(SppEcho.DEFAULT_SERVICE_UUID), EchoSize.MEDIUM.bytes)),
            client.calls,
        )

        client.gate!!.complete(Unit)

        assertEquals(EchoRunState.Done(report(EchoSize.MEDIUM.bytes)), viewModel.uiState.value.run)
        assertTrue(viewModel.uiState.value.canRun)
    }

    @Test
    fun `a failed run shows the reason`() = runTest {
        client.result = Result.failure(IOException("read failed, socket might closed or timeout"))
        viewModel.selectDevice("00:11:22:33:44:55")

        viewModel.runEcho()

        assertEquals(
            EchoRunState.Failed("read failed, socket might closed or timeout"),
            viewModel.uiState.value.run,
        )
    }

    @Test
    fun `a selected device that is no longer paired is deselected`() {
        viewModel.selectDevice("66:77:88:99:AA:BB")
        bluetooth.devices = bluetooth.devices.take(1)

        viewModel.refreshDevices()

        assertNull(viewModel.uiState.value.selectedAddress)
    }

    private companion object {
        fun report(bytes: Int) = EchoReport(
            bytesSent = bytes,
            bytesReceived = bytes,
            firstMismatchAt = null,
            roundTripMillis = 40,
            totalMillis = 2_000,
        )
    }
}

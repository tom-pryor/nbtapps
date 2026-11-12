// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import com.nbtapps.store.model.EchoReport
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.IOException
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Bluetooth serial (SPP / RFCOMM) echo test against a service on a paired device: the phone opens
 * the link to the service's UUID, and the other side sends everything straight back.
 */
interface SppEchoClient {
    /**
     * Connects to [serviceUuid] on the paired device at [address], sends [totalBytes] and checks the
     * echo. [onProgress] gets the number of bytes echoed so far and may be called from any thread.
     */
    suspend fun runEcho(
        address: String,
        serviceUuid: UUID,
        totalBytes: Int,
        onProgress: (received: Int) -> Unit,
    ): Result<EchoReport>
}

@Singleton
class BluetoothSppEchoClient @Inject constructor(
    @ApplicationContext private val context: Context,
    private val bluetooth: PhoneBluetooth,
) : SppEchoClient {

    private val adapter: BluetoothAdapter?
        get() = context.getSystemService(BluetoothManager::class.java)?.adapter

    @SuppressLint("MissingPermission") // checked through bluetooth.access()
    override suspend fun runEcho(
        address: String,
        serviceUuid: UUID,
        totalBytes: Int,
        onProgress: (received: Int) -> Unit,
    ): Result<EchoReport> = withContext(Dispatchers.IO) {
        when (bluetooth.access()) {
            BluetoothAccess.MISSING -> return@withContext failure("This phone has no Bluetooth")
            BluetoothAccess.NOT_ALLOWED -> return@withContext failure("The Nearby devices permission isn't granted")
            BluetoothAccess.OFF -> return@withContext failure("Bluetooth is off")
            BluetoothAccess.ON -> Unit
        }
        val adapter = adapter ?: return@withContext failure("This phone has no Bluetooth")
        try {
            val device = adapter.getRemoteDevice(address)
            // The car has one serial link per phone, and another app on the phone may hold it for some
            // seconds at a time: a connect that fails is tried again for a while.
            val socket = connectWithRetry(CONNECT_WINDOW_MILLIS, CONNECT_PAUSE_MILLIS, System::currentTimeMillis) {
                val candidate = device.createRfcommSocketToServiceRecord(serviceUuid)
                try {
                    candidate.connect()
                    candidate
                } catch (e: IOException) {
                    candidate.close()
                    throw e
                }
            }
            socket.use {
                coroutineScope {
                    // The exchange blocks in read(); closing the socket is the only way to end it.
                    val watchdog = launch {
                        delay(timeoutMillis(totalBytes))
                        socket.close()
                    }
                    try {
                        Result.success(EchoExchange(socket.inputStream, socket.outputStream).run(totalBytes, onProgress))
                    } finally {
                        watchdog.cancel()
                    }
                }
            }
        } catch (e: IOException) {
            Result.failure(e)
        } catch (e: SecurityException) {
            Result.failure(e)
        } catch (e: IllegalArgumentException) {
            Result.failure(e) // not a Bluetooth address
        }
    }

    private fun failure(message: String): Result<EchoReport> = Result.failure(IOException(message))

    private companion object {
        /** How long a connect is tried again: longer than another app holds the car's link unanswered. */
        const val CONNECT_WINDOW_MILLIS = 20_000L
        const val CONNECT_PAUSE_MILLIS = 700L

        /** A fixed allowance plus the time the data would take at a slow 10 KB/s. */
        fun timeoutMillis(totalBytes: Int): Long = 15_000L + totalBytes / 10L
    }
}

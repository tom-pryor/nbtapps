// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Context
import com.nbtapps.store.data.PhoneBluetooth
import com.nbtapps.store.data.serialUnavailable
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import javax.inject.Inject
import javax.inject.Singleton

/** The phone's own Bluetooth, for [BluetoothSerialLinkConnector]. */
@Singleton
class AndroidSerialRadio @Inject constructor(
    @ApplicationContext private val context: Context,
    private val bluetooth: PhoneBluetooth,
) : SerialRadio {

    override fun unavailable(address: String): String? = bluetooth.serialUnavailable(address)

    @SuppressLint("MissingPermission") // checked by unavailable()
    override fun socket(address: String, uuid: UUID): SerialSocket = try {
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: throw IOException("This phone has no Bluetooth")
        // The secure kind: the link is authenticated and encrypted by the pairing.
        Socket(adapter.getRemoteDevice(address.uppercase()).createRfcommSocketToServiceRecord(uuid))
    } catch (e: SecurityException) {
        throw IOException("The Nearby devices permission isn't granted", e)
    } catch (e: IllegalArgumentException) {
        throw IOException("The Bluetooth address saved for this head unit isn't one", e)
    }

    private class Socket(private val socket: BluetoothSocket) : SerialSocket {
        override val input: InputStream get() = socket.inputStream
        override val output: OutputStream get() = socket.outputStream

        @SuppressLint("MissingPermission") // checked by unavailable()
        override fun connect() = try {
            socket.connect()
        } catch (e: SecurityException) {
            throw IOException("The Nearby devices permission isn't granted", e)
        }

        override fun close() = socket.close()
    }
}

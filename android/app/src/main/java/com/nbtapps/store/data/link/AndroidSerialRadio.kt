// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothServerSocket
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

    /**
     * The secure kind, as [socket]'s: only a device paired with the phone gets through, and the link is encrypted.
     * Never the generic serial port's UUID, which other programs on the car dial.
     */
    @SuppressLint("MissingPermission") // a SecurityException says it isn't granted
    override fun listen(name: String, uuid: UUID): SerialServer = try {
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: throw IOException("This phone has no Bluetooth")
        Server(adapter.listenUsingRfcommWithServiceRecord(name, uuid))
    } catch (e: SecurityException) {
        throw IOException("The Nearby devices permission isn't granted", e)
    }

    private class Server(private val server: BluetoothServerSocket) : SerialServer {
        override fun accept(): AcceptedSocket = try {
            val socket = server.accept()
            AcceptedSocket(Socket(socket), socket.remoteDevice.address)
        } catch (e: SecurityException) {
            throw IOException("The Nearby devices permission isn't granted", e)
        }

        override fun close() {
            try {
                server.close()
            } catch (e: IOException) {
                // It no longer listens either way.
            }
        }
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

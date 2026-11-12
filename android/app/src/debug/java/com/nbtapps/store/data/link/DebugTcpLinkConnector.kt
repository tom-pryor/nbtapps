// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.model.Endpoint
import com.nbtapps.store.model.TcpEndpoint
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Plain TCP to an agent, with no encryption and no frame MAC: the link of the test bench. The agent at the other
 * end is the bench build of `nbtagent`, started with `--debug-tcp PORT` in a Docker container by the agent
 * repo's `tools/bench.py`, which leaves the port on its machine's loopback address only. After
 * `adb reverse tcp:PORT tcp:PORT` an emulator, or a phone on USB, reaches it as `127.0.0.1:PORT`. (An emulator's own name for the host, `10.0.2.2`, is not relied on: where the
 * emulator's Wi-Fi is the app's network, TCP to it can time out.)
 *
 * The agent asks for pairing and `auth` on this link as it does on Bluetooth serial, so the app's whole path can
 * be tried on the bench; but the pairing key crosses in the clear when it is issued, and anything on the way can
 * read and change what follows. So this must never reach a real head unit. It is in the debug source
 * set, beside the test keys, and the release build has no such class: it reaches a head unit over Bluetooth
 * serial only ([BluetoothSerialLinkConnector]) until authenticated TCP exists.
 */
class DebugTcpLinkConnector : LinkConnector {

    override fun supports(endpoint: Endpoint): Boolean = endpoint is TcpEndpoint

    override suspend fun connect(endpoint: Endpoint): Link = withContext(Dispatchers.IO) {
        val tcp = endpoint as? TcpEndpoint ?: throw IOException("This debug build reaches an agent over TCP only")
        // The phone's own routing: the bench is at 127.0.0.1 through `adb reverse`, on no network of the phone's.
        // The name is looked up here, not by the socket: a name that is not known then fails with words for the
        // user, where the socket's failure is the bare name.
        val address = InetAddress.getByName(tcp.host)
        val socket = Socket()
        try {
            // Messages are small and each waits for its answer: they must not sit in the socket to be batched.
            socket.tcpNoDelay = true
            socket.connect(InetSocketAddress(address, tcp.port), CONNECT_TIMEOUT_MS)
        } catch (e: IOException) {
            socket.close()
            throw e
        }
        TcpLink(socket)
    }

    private class TcpLink(private val socket: Socket) : Link {
        override val kind = "tcp"
        override val input: InputStream = socket.getInputStream()
        override val output: OutputStream = socket.getOutputStream()

        /** Closing the socket also fails a read that is blocked on it. */
        override fun close() = socket.close()
    }

    private companion object {
        const val CONNECT_TIMEOUT_MS = 5_000
    }
}

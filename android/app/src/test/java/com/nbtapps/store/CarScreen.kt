// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store

import com.nbtapps.store.data.link.Link
import com.nbtapps.store.data.link.Peer
import com.nbtapps.store.data.link.PeerHandler
import java.io.Closeable
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.net.UnixDomainSocketAddress
import java.nio.ByteBuffer
import java.nio.channels.SocketChannel
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

/** A link over the agent's local socket at [path]: the way in that the head unit's own programs have. */
class LocalSocketLink(path: File) : Link {
    private val channel = SocketChannel.open(UnixDomainSocketAddress.of(path.toPath()))
    override val kind = "local"

    override val input = object : InputStream() {
        override fun read(): Int = throw UnsupportedOperationException()
        override fun read(buffer: ByteArray, offset: Int, length: Int): Int = channel.read(ByteBuffer.wrap(buffer, offset, length))
    }

    override val output = object : OutputStream() {
        override fun write(byte: Int) = write(byteArrayOf(byte.toByte()), 0, 1)
        override fun write(bytes: ByteArray, offset: Int, length: Int) {
            val buffer = ByteBuffer.wrap(bytes, offset, length)
            while (buffer.hasRemaining()) channel.write(buffer)
        }
    }

    override fun close() = channel.close()
}

/**
 * The car's side of pairing, for a test against the real agent ([AgentBench]): a controller on the agent's
 * local socket, as the head unit's plugin and `nbtctl` are. It does what the plugin's screens do: opens the
 * pairing window and is told the code, and tells the agent to forget its phones. Only a local link may.
 *
 * The window stays open for as long as this does, or until it is used, so a test closes it last.
 */
class CarScreen(socket: File) : PeerHandler, Closeable {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val peer = Peer(LocalSocketLink(socket), dialled = true, handler = this)
    private val greeting = Mutex()
    private var greeted = false

    init {
        scope.launch { peer.run() }
    }

    /** "Pair a phone": opens the pairing window, in place of one that is open, and returns the code the screen shows. */
    suspend fun pairingCode(): String = ask("pairing.open").getValue("code").jsonPrimitive.content

    /** "Forget all phones". Returns how many there were. */
    suspend fun forgetAll(): Int =
        ask("phones.forget", buildJsonObject { put("all", true) }).getValue("forgotten").jsonPrimitive.content.toInt()

    /** The ids of the phones the agent holds a key for. */
    suspend fun phoneIds(): List<String> =
        (ask("phones.list").getValue("phones") as JsonArray).map { it.jsonObject.getValue("phoneId").jsonPrimitive.content }

    private suspend fun ask(method: String, params: JsonObject = JsonObject(emptyMap())): JsonObject {
        greeting.withLock {
            if (!greeted) {
                // As `nbtctl` says it: a controller that is no phone, so it has no id and no name.
                peer.request("hello", buildJsonObject {
                    put("protocolMin", 1)
                    put("protocolMax", 1)
                    put("role", "cli")
                    put("maxFrame", 65536)
                    put("capabilities", buildJsonArray {})
                })
                greeted = true
            }
        }
        return peer.request(method, params)
    }

    override suspend fun onRequest(peer: Peer, id: Long, method: String, params: JsonObject) {
        if (method == "ping") peer.respond(id, JsonObject(emptyMap())) else peer.respondError(id, "unsupported", "a test's screen answers ping only")
    }

    /** `pairing.closed` and the events of jobs come here too, and nothing waits for them. */
    override suspend fun onEvent(peer: Peer, name: String, seq: Long?, params: JsonObject) = Unit

    override fun close() {
        peer.close("the test is over")
        scope.cancel()
    }
}

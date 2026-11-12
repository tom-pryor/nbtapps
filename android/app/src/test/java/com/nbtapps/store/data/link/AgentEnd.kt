// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.data.json.StrictJson
import kotlinx.serialization.json.JsonObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals

/** The agent's end of a test link, driven by hand: a test says what the agent reads and what it sends. */
class AgentEnd(private val link: Link) {
    private val reader = FrameReader(link)

    fun expectMagic() = assertArrayEquals(FrameCodec.MAGIC, reader.raw(FrameCodec.MAGIC.size))

    fun frame(): Frame = requireNotNull(reader.next()) { "the phone closed the link" }

    /** Whether the phone has closed the link: true once nothing more comes. */
    fun closedByPhone(): Boolean = reader.next() == null

    fun message(): JsonObject {
        val frame = frame()
        assertEquals("a MSG frame", FrameCodec.TYPE_MSG, frame.type)
        return StrictJson.parseObject(frame.payload)
    }

    fun send(json: String) = raw(FrameCodec.encode(FrameCodec.TYPE_MSG, 0, 0, json.encodeToByteArray()))

    fun raw(bytes: ByteArray) {
        link.output.write(bytes)
        link.output.flush()
    }

    fun close() = link.close()

    /** Reads the phone's `hello` and answers it as an agent does. Returns what the phone sent as its params. */
    fun answerHello(maxFrame: Int = 65536, headUnitId: String = "000102030405060708090a0b0c0d0e0f"): JsonObject {
        expectMagic()
        val hello = message()
        assertEquals("\"hello\"", hello["method"].toString())
        send(
            """{"id":${hello["id"]},"result":{"protocolMin":1,"protocolMax":1,"role":"agent","headUnitId":"$headUnitId",""" +
                """"agentVersion":"1.0.0","maxFrame":$maxFrame,"capabilities":[]}}""",
        )
        return hello["params"] as JsonObject
    }
}

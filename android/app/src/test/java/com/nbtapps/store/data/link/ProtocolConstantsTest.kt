// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

import com.nbtapps.store.AgentVectors
import com.nbtapps.store.data.packages.ManifestParser
import java.util.UUID
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * [ProtocolConstants] holds what the agent repo's `spec/protocol/constants.txt` holds, name for name. The copy
 * under the test resources is the one read; `AgentVectorsInSyncTest` says when that copy has fallen behind. The
 * agent checks its own code against the same file, so a value changed on one side only fails here or there.
 */
class ProtocolConstantsTest {

    /** The file: one `name value` to a line, each name once, `#` lines as comments. */
    private val file: Map<String, String> = LinkedHashMap<String, String>().also { values ->
        for ((index, line) in AgentVectors.file("protocol/constants.txt").readLines().withIndex()) {
            if (line.isEmpty() || line.startsWith("#")) continue
            val parts = line.split(' ')
            assertTrue("line ${index + 1} is not a name, a space and a value: $line", parts.size == 2 && parts.none { it.isEmpty() })
            assertFalse("${parts[0]} is there twice", parts[0] in values)
            values[parts[0]] = parts[1]
        }
    }

    private val app = mapOf(
        "protocol.version" to ProtocolConstants.PROTOCOL_VERSION.toString(),
        "frame.maxPayload" to ProtocolConstants.FRAME_MAX_PAYLOAD.toString(),
        "frame.minPayload" to ProtocolConstants.FRAME_MIN_PAYLOAD.toString(),
        "serial.serviceName" to ProtocolConstants.SERIAL_SERVICE_NAME,
        "serial.serviceUuid" to ProtocolConstants.SERIAL_SERVICE_UUID,
        "serial.maxFrame" to ProtocolConstants.SERIAL_MAX_FRAME.toString(),
        "serial.pingAfterMs" to ProtocolConstants.SERIAL_PING_AFTER_MS.toString(),
        "serial.silenceMs" to ProtocolConstants.SERIAL_SILENCE_MS.toString(),
        "serial.decideMs" to ProtocolConstants.SERIAL_DECIDE_MS.toString(),
        "serial.lingerMs" to ProtocolConstants.SERIAL_LINGER_MS.toString(),
        "serial.knockMagic" to ProtocolConstants.SERIAL_KNOCK_MAGIC,
        "serial.knockAttempts" to ProtocolConstants.SERIAL_KNOCK_ATTEMPTS.toString(),
        "serial.knockGapMs" to ProtocolConstants.SERIAL_KNOCK_GAP_MS.toString(),
        "serial.knockHoldMs" to ProtocolConstants.SERIAL_KNOCK_HOLD_MS.toString(),
        "pairing.codeDigits" to ProtocolConstants.PAIRING_CODE_DIGITS.toString(),
        "pairing.windowSeconds" to ProtocolConstants.PAIRING_WINDOW_SECONDS.toString(),
        "pairing.tries" to ProtocolConstants.PAIRING_TRIES.toString(),
        "pairing.pauseSeconds" to ProtocolConstants.PAIRING_PAUSE_SECONDS.toString(),
        "auth.nonceBytes" to ProtocolConstants.AUTH_NONCE_BYTES.toString(),
        "capability.autostart" to ProtocolConstants.CAPABILITY_AUTOSTART,
        "capability.detachedSignature" to ProtocolConstants.CAPABILITY_DETACHED_SIGNATURE,
        "capability.files" to ProtocolConstants.CAPABILITY_FILES,
        "capability.checks" to ProtocolConstants.CAPABILITY_CHECKS,
        "capability.serialReset" to ProtocolConstants.CAPABILITY_SERIAL_RESET,
        "capability.serialDial" to ProtocolConstants.CAPABILITY_SERIAL_DIAL,
        "capability.serialKnock" to ProtocolConstants.CAPABILITY_SERIAL_KNOCK,
        "capability.settings" to ProtocolConstants.CAPABILITY_SETTINGS,
        "settings.maxValueBytes" to ProtocolConstants.SETTINGS_MAX_VALUE_BYTES.toString(),
    )

    /** What only the agent needs. Nothing at present: the app reads manifests, and so needs the service's name too. */
    private val agentOnly = emptySet<String>()

    @Test
    fun `the app holds what the file holds, name for name`() {
        for ((name, value) in app) assertEquals(name, file[name], value)
        // A name that is new in the file has to be taken up here, or be put down as the agent's alone.
        assertEquals(emptySet<String>(), file.keys - app.keys - agentOnly)
        assertTrue(file.keys.containsAll(agentOnly))
    }

    @Test
    fun `the values fit each other, and the code uses them`() {
        assertTrue(ProtocolConstants.SERIAL_MAX_FRAME in ProtocolConstants.FRAME_MIN_PAYLOAD..ProtocolConstants.FRAME_MAX_PAYLOAD)
        // A ping, and time for its answer.
        assertTrue(ProtocolConstants.SERIAL_PING_AFTER_MS > 0 && 2 * ProtocolConstants.SERIAL_PING_AFTER_MS <= ProtocolConstants.SERIAL_SILENCE_MS)
        // The agent decides on a link long before it would ping it.
        assertTrue(ProtocolConstants.SERIAL_DECIDE_MS in 1 until ProtocolConstants.SERIAL_PING_AFTER_MS)
        // The agent's log says a link is still up, after its own end closed, well before the app's keep-alive would
        // end it. So the app has to end the link when `link.closing` comes, which SerialSessionTest holds it to.
        assertTrue(ProtocolConstants.SERIAL_LINGER_MS in 1 until ProtocolConstants.SERIAL_SILENCE_MS)
        // A knock is more than one connect, since one may fail on the phone while another app's link is up, and the
        // one that comes up is held past the agent's decide step; the knock's magic bytes are four, as the link's are,
        // and not the same.
        assertTrue(ProtocolConstants.SERIAL_KNOCK_ATTEMPTS >= 2 && ProtocolConstants.SERIAL_KNOCK_GAP_MS > 0)
        assertTrue(ProtocolConstants.SERIAL_KNOCK_HOLD_MS > ProtocolConstants.SERIAL_DECIDE_MS)
        assertEquals(ProtocolConstants.SERIAL_KNOCK_MAGIC, String(FrameCodec.KNOCK_MAGIC, Charsets.US_ASCII))
        assertFalse(FrameCodec.KNOCK_MAGIC.contentEquals(FrameCodec.MAGIC))
        // A package's serial service may have neither the agent's name nor the car's.
        assertFalse(ManifestParser.isSerialServiceName(ProtocolConstants.SERIAL_SERVICE_NAME))
        assertFalse(ManifestParser.isSerialServiceName("SPP"))
        assertTrue(ManifestParser.isSerialServiceName(ProtocolConstants.SERIAL_SERVICE_NAME + "2"))

        // A code the user can type, a window long enough to type it in, and a nonce the client makes at that size.
        assertTrue(ProtocolConstants.PAIRING_CODE_DIGITS in 4..9 && ProtocolConstants.PAIRING_TRIES >= 1)
        assertTrue(ProtocolConstants.PAIRING_WINDOW_SECONDS >= 60 && ProtocolConstants.PAIRING_PAUSE_SECONDS > 0)
        assertEquals(ProtocolConstants.AUTH_NONCE_BYTES, AgentClient.newNonce().size)
        assertFalse(AgentClient.newNonce().contentEquals(AgentClient.newNonce()))

        // A setting's value holds a select's value (64 characters) and fits a serial frame many times over.
        assertTrue(ProtocolConstants.SETTINGS_MAX_VALUE_BYTES in 64..ProtocolConstants.SERIAL_MAX_FRAME / 4)

        assertEquals(ProtocolConstants.FRAME_MAX_PAYLOAD, FrameCodec.MAX_PAYLOAD)
        assertEquals(ProtocolConstants.FRAME_MIN_PAYLOAD, FrameCodec.MIN_PAYLOAD)
        assertEquals(ProtocolConstants.PROTOCOL_VERSION, AgentClient.PROTOCOL_VERSION)
        // The UUID the connector dials is the file's, in the form the file writes it.
        assertEquals(UUID.fromString(file["serial.serviceUuid"]), BluetoothSerialLinkConnector.SERVICE_UUID)
        assertEquals(file["serial.serviceUuid"], BluetoothSerialLinkConnector.SERVICE_UUID.toString().uppercase())
    }
}

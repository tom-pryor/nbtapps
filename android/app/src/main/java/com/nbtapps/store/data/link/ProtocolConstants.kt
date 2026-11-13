// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
package com.nbtapps.store.data.link

/**
 * The names and numbers the agent and this app must both hold, and hold alike: the agent repo's
 * `spec/protocol/constants.txt`, as this code has them. That file says what each one is.
 *
 * `ProtocolConstantsTest` reads the copy of the file under the test resources and fails when the two differ, so
 * a value changes there and here or in neither. The agent has the same check against its own code.
 */
object ProtocolConstants {
    const val PROTOCOL_VERSION = 1

    /** A frame's payload in bytes: the most there is, and the least `hello` may ask for. */
    const val FRAME_MAX_PAYLOAD = 65536
    const val FRAME_MIN_PAYLOAD = 1024

    /**
     * The agent's Bluetooth serial service on the car: the name it is registered under there, which no package's
     * service may take, and what the phone connects to.
     */
    const val SERIAL_SERVICE_NAME = "NBTApp"
    const val SERIAL_SERVICE_UUID = "58876F35-7F3F-4592-B65D-9912589A4F51"

    /** What each side gives, at most, as its `maxFrame` on a serial link. */
    const val SERIAL_MAX_FRAME = 4096

    /** A serial link is pinged after this long with nothing heard, in milliseconds, and ended after the second: see [KeepAlive]. */
    const val SERIAL_PING_AFTER_MS = 10_000L
    const val SERIAL_SILENCE_MS = 30_000L

    /**
     * How long the agent waits for the magic bytes on a serial link, in milliseconds from when it opened the
     * link, before it lets the link go as another program's. The phone cannot see that moment, so it sends them
     * the instant its link is up: see [BluetoothSerialLinkConnector].
     */
    const val SERIAL_DECIDE_MS = 500L

    /**
     * How long the agent keeps a serial link's node open, in milliseconds, once it has closed its own end of the
     * link: the phone made the link, and the phone ends it. The app does so the moment `link.closing` arrives
     * ([AgentSession]), which is well inside this. Left to its keep-alive it would hold the link for
     * [SERIAL_SILENCE_MS], and have the node closed under it.
     */
    const val SERIAL_LINGER_MS = 5_000L

    /** Pairing a phone: the car's screen shows a code of this many digits, which the user types into the app. */
    const val PAIRING_CODE_DIGITS = 6

    /**
     * The code is good for the first of these, in seconds, from when the car's screen asked for it. The agent takes
     * this many wrong codes, the last of which closes the window, and for the last of the three, in seconds, it
     * then opens no new one.
     */
    const val PAIRING_WINDOW_SECONDS = 120
    const val PAIRING_TRIES = 3
    const val PAIRING_PAUSE_SECONDS = 30

    /** `auth`: each side's nonce, in bytes. It travels as twice as many hex characters. */
    const val AUTH_NONCE_BYTES = 16

    /**
     * A capability, as an agent lists it in `hello`: it has `autostart.get` and `autostart.set`, which say and set
     * whether the agent is started with the head unit. An agent that does not list it has no such switch.
     */
    const val CAPABILITY_AUTOSTART = "autostart"

    /**
     * A capability: the agent installs an offer with the offer's `signature`, checked over the manifest, and does
     * not read the archive's `manifest.sig`. That is what a package needs whose author signed it with a key of their
     * own and whose catalogue gives a detached signature for it. An agent that does not list it refuses such a
     * package at install with `bad-signature`.
     */
    const val CAPABILITY_DETACHED_SIGNATURE = "detached-signature"

    /**
     * A capability: the agent has `files.list`, `files.fetch` and `files.cancel`, and sends the logs of an installed
     * package, those its manifest names in `logs`, and its own. An agent that does not list it has none to send.
     */
    const val CAPABILITY_FILES = "files"
}

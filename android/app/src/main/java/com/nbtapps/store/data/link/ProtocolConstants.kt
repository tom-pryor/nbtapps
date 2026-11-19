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
     * The phone made the link, and the phone ends it. Once the agent has closed its own end of a serial link it keeps
     * the link's node open and reads it, dropping what comes, until the phone has ended the link, however long that
     * takes: closing a node does not end its link. When the link is still up this long after the agent's close, in
     * milliseconds, the agent's log says so. The app ends its link the moment `link.closing` arrives ([AgentSession]),
     * well inside this; left to its keep-alive it would hold the link for [SERIAL_SILENCE_MS].
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

    /**
     * A capability: the agent has `checks.run` and `checks.fix`. It looks for what was put onto the head unit by
     * hand, outside this system, and stands in the way of a package or of the head unit, and removes it when the
     * phone asks, with the token of the list the user was shown. An agent that does not list it has no checks.
     */
    const val CAPABILITY_CHECKS = "checks"

    /**
     * A capability: the agent has `serial.reset`, which asks the car's Bluetooth service to end the phone's Bluetooth
     * serial link: at once if a serial link's node is open, and otherwise at the open of the next one. Only an agent
     * that takes Bluetooth serial links lists it; one that does not list it answers `serial.reset` with `unsupported`.
     */
    const val CAPABILITY_SERIAL_RESET = "serial-reset"

    /**
     * A capability: the agent has the car dial a phone of its own. When the phone is connected to the car and paired
     * with it, the car opens a serial link to the agent's service UUID on the phone, which this app takes if it
     * listens there. The link is the agent's as one the phone opened is: it opens with the phone's magic bytes, and
     * `hello` follows. Only an agent whose Bluetooth serial listener dials lists it.
     */
    const val CAPABILITY_SERIAL_DIAL = "serial-dial"

    /**
     * A capability: the agent has `settings.get` and `settings.set`, which give and change the settings an installed
     * package's manifest names in `settings`. One that does not list it answers them with `unsupported`.
     */
    const val CAPABILITY_SETTINGS = "settings"

    /**
     * The most bytes one setting's value has, in UTF-8, as it stands in the package's settings file: so also the most
     * a `text` option's `maxLength` may be.
     */
    const val SETTINGS_MAX_VALUE_BYTES = 255

    /**
     * A capability: the agent has channels. A controller opens a named channel of an installed package with
     * `channel.open`, and messages pass both ways as DATA frames on the channel's stream, with credits
     * (`channel.credit`), until either side closes it (`channel.close`); `channel.get` and `channel.set` say and set
     * whether a switched channel is on. One that does not list it answers those methods with `unsupported`.
     */
    const val CAPABILITY_CHANNELS = "channels"

    /**
     * Stream numbers: a files stream and the stream of a `blob.fetch` take the numbers from 1 to [STREAM_LAST], and
     * start again at 1 after it; a channel takes the numbers from [CHANNEL_FIRST_STREAM] to 65,535.
     */
    const val STREAM_LAST = 32767
    const val CHANNEL_FIRST_STREAM = 32768

    /** A channel's message is one DATA frame, with the last flag set, of 1 to this many bytes, on every link. */
    const val CHANNEL_MAX_MESSAGE = 4096

    /** The messages each side of a channel may send before the other grants more with `channel.credit`, at first. */
    const val CHANNEL_CREDIT = 16

    /** The most channels open on one link. */
    const val CHANNEL_PER_LINK = 8

    /**
     * A channel whose program the agent starts on demand: the agent tries to connect to it every [CHANNEL_START_RETRY_MS]
     * for [CHANNEL_START_WAIT_MS], and answers `channel.open` with `unavailable` when the program has not taken the
     * connection by then. So the answer to `channel.open` may take that long.
     */
    const val CHANNEL_START_WAIT_MS = 5_000L
    const val CHANNEL_START_RETRY_MS = 100L
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PROTOCOL_CONSTANTS_HPP
#define NBT_AGENT_PROTOCOL_CONSTANTS_HPP

// The names and numbers that the agent and a phone app must both have: spec/protocol/constants.txt, as this code
// has them. The file says what each one is. tests/agent/test_constants.cpp fails when the two differ, so a value
// is changed in both places or in neither; the phone app has the same file and a test of its own.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

namespace nbt {

const int kProtocolVersion = 1;

const uint32_t kMaxFramePayload = 65536;   // the most `hello` may set
const uint32_t kMinFramePayload = 1024;    // the least `hello` may set

const char* const kSerialServiceName = "NBTApp";
const char* const kSerialServiceUuid = "58876F35-7F3F-4592-B65D-9912589A4F51";
const uint32_t kSerialMaxFrame = 4096;
const int64_t kSerialPingAfterMs = 10000;
const int64_t kSerialSilenceMs = 30000;
const int64_t kSerialDecideMs = 500;
const int64_t kSerialLingerMs = 5000;

// The knock (constants.txt says what each is): the bytes a phone app's knock link carries, and nothing else
// (frame.hpp has them as bytes); and how the app knocks.
const char* const kSerialKnockMagic = "NBTK";
const unsigned kSerialKnockAttempts = 5;
const int64_t kSerialKnockGapMs = 250;
const int64_t kSerialKnockHoldMs = 1000;

const unsigned kPairingCodeDigits = 6;
const int64_t kPairingWindowSeconds = 120;
const unsigned kPairingTries = 3;
const int64_t kPairingPauseSeconds = 30;

const size_t kAuthNonceBytes = 16;

// A capability, as `hello` names it: this agent has autostart.get and autostart.set.
const char* const kCapabilityAutostart = "autostart";
// A capability: this agent installs an offer with the offer's signature, which may be a catalogue's detached
// signature over the manifest, and does not read the archive's manifest.sig.
const char* const kCapabilityDetachedSignature = "detached-signature";
// A capability: this agent has files.list, files.fetch and files.cancel, and sends a controller the logs of an
// installed package.
const char* const kCapabilityFiles = "files";
// A capability: this agent has checks.run and checks.fix, the compatibility checks (checks.hpp).
const char* const kCapabilityChecks = "checks";
// A capability: this agent has serial.reset, which has the car end the phone's Bluetooth serial link
// (serial_reset.hpp). Only an agent with a Bluetooth serial listener lists it.
const char* const kCapabilitySerialReset = "serial-reset";
// A capability: the car dials a phone of this agent's that is connected to it and paired with it, on the agent's
// UUID, and the agent takes that link as it takes one the phone opens. Only an agent whose Bluetooth serial listener
// has the dial on (nbtagent --dial) lists it.
const char* const kCapabilitySerialDial = "serial-dial";
// A capability: the car dials a phone of this agent's when that phone's app knocks (constants.txt,
// serial.knockMagic), and, but for the pairing window and a version on trial, not otherwise. Only an agent whose
// Bluetooth serial listener has the dial on with knocks (nbtagent --dial-knock) lists it, and it lists serial-dial too.
const char* const kCapabilitySerialKnock = "serial-knock";
// A capability: this agent has settings.get and settings.set, for the settings a package's manifest names
// (settings.hpp).
const char* const kCapabilitySettings = "settings";

// The most bytes one setting's value has in a package's settings file, and so the most a `text` option's maxLength
// may be.
const size_t kSettingsMaxValueBytes = 255;

// A capability: this agent has channel.open, channel.get and channel.set, and the DATA and events of a channel
// (agent.hpp, "Channels").
const char* const kCapabilityChannels = "channels";
// The last stream number of a files stream or a blob.fetch; the first of a channel. A channel's numbers run from
// there to 65,535.
const uint16_t kStreamLast = 32767;
const uint16_t kChannelFirstStream = 32768;
// The most bytes of one channel message, which is one DATA frame, on every link.
const size_t kChannelMaxMessage = 4096;
// The messages each side may send before the other grants more, at first.
const unsigned kChannelCredit = 16;
// The most channels open on one link.
const unsigned kChannelPerLink = 8;
// How long the agent tries to connect to a program it has started, and how often.
const int64_t kChannelStartWaitMs = 5000;
const int64_t kChannelStartRetryMs = 100;

}  // namespace nbt

#endif

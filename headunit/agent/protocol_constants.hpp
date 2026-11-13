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

}  // namespace nbt

#endif

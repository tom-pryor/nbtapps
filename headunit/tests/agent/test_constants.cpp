// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test: agent/protocol_constants.hpp holds what spec/protocol/constants.txt holds, name for name. The file is
// what the phone app's tests read too, so a value changed on one side only fails here or there.
//
// usage: test_constants [constants file]

#include "../../agent/protocol_constants.hpp"

#include <stdio.h>

#include <map>
#include <string>

#include "../../agent/serial_link.hpp"
#include "test_util.hpp"

using namespace nbt;

static std::string num(long long value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    return text;
}

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "spec/protocol/constants.txt";
    std::string text = testutil::readFile(path);

    // The file: one "name value" to a line, each name once.
    std::map<std::string, std::string> file;
    size_t at = 0;
    int number = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        CHECK_IN(path + ": the last line has no line end", end != std::string::npos);
        std::string line = text.substr(at, end - at);
        at = end + 1;
        ++number;
        if (line.empty() || line[0] == '#') continue;
        std::string where = path + ":" + num(number);
        size_t space = line.find(' ');
        CHECK_IN(where, space != std::string::npos && space > 0 && space + 1 < line.size());
        CHECK_IN(where, line.find(' ', space + 1) == std::string::npos && line.find('\t') == std::string::npos &&
                            line.find('\r') == std::string::npos);
        std::string name = line.substr(0, space);
        CHECK_IN(where + ": " + name + " is there twice", file.count(name) == 0);
        file[name] = line.substr(space + 1);
    }

    // The code.
    std::map<std::string, std::string> code;
    code["protocol.version"] = num(kProtocolVersion);
    code["frame.maxPayload"] = num(kMaxFramePayload);
    code["frame.minPayload"] = num(kMinFramePayload);
    code["serial.serviceName"] = kSerialServiceName;
    code["serial.serviceUuid"] = kSerialServiceUuid;
    code["serial.maxFrame"] = num(kSerialMaxFrame);
    code["serial.pingAfterMs"] = num(kSerialPingAfterMs);
    code["serial.silenceMs"] = num(kSerialSilenceMs);
    code["serial.decideMs"] = num(kSerialDecideMs);
    code["serial.lingerMs"] = num(kSerialLingerMs);
    code["pairing.codeDigits"] = num(kPairingCodeDigits);
    code["pairing.windowSeconds"] = num(kPairingWindowSeconds);
    code["pairing.tries"] = num(kPairingTries);
    code["pairing.pauseSeconds"] = num(kPairingPauseSeconds);
    code["auth.nonceBytes"] = num((long long)kAuthNonceBytes);
    code["capability.autostart"] = kCapabilityAutostart;
    code["capability.detachedSignature"] = kCapabilityDetachedSignature;

    for (std::map<std::string, std::string>::const_iterator it = code.begin(); it != code.end(); ++it) {
        CHECK_IN(path + " has no " + it->first, file.count(it->first) != 0);
        CHECK_IN(it->first + ": the file says " + file[it->first] + ", the agent's code " + it->second, file[it->first] == it->second);
    }
    for (std::map<std::string, std::string>::const_iterator it = file.begin(); it != file.end(); ++it) {
        CHECK_IN(it->first + " is in " + path + " and not in agent/protocol_constants.hpp", code.count(it->first) != 0);
    }

    // What the values have to be to each other for the rest of the code to make sense.
    CHECK(kMinFramePayload <= kSerialMaxFrame && kSerialMaxFrame <= kMaxFramePayload);
    CHECK(kSerialPingAfterMs > 0 && 2 * kSerialPingAfterMs <= kSerialSilenceMs);   // a ping, and time for its answer
    CHECK(kSerialDecideMs > 0 && kSerialDecideMs < kSerialPingAfterMs);
    // Time for a phone to hear link.closing and end its link, and less than the silence a phone ends a link for.
    CHECK(kSerialLingerMs >= 1000 && kSerialLingerMs < kSerialSilenceMs);
    // A code that three guesses in a million find, and a window that a person has time to type it in.
    CHECK(kPairingCodeDigits >= 6 && kPairingCodeDigits <= 9 && kPairingTries >= 1 && kPairingTries <= 5);
    CHECK(kPairingWindowSeconds >= 30 && kPairingPauseSeconds >= 1);
    CHECK(kAuthNonceBytes >= 16);
    // The listener registers exactly this service, and decides on a link in exactly this time.
    SerialConfig config;
    CHECK(config.name == kSerialServiceName && config.uuid == kSerialServiceUuid);
    CHECK(config.decideMs == (unsigned)kSerialDecideMs && config.lingerMs == (unsigned)kSerialLingerMs);
    std::string asGiven;   // to the stack
    CHECK(btspp::normaliseUuid(kSerialServiceUuid, &asGiven) && asGiven == kSerialServiceUuid);
    CHECK(btspp::isServiceName(kSerialServiceName));

    printf("test_constants: ok (%u names)\n", (unsigned)code.size());
    return 0;
}

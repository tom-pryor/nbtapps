// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The relay's key-line rule and keys message (ssh/relay/key_lines.hpp): lines that pass and lines that do not.
// The keys are invented: their bytes count up from 0 or 1, so only their layout is that of a real key. The base64
// below was written by another encoder (Python's), so the rule is not checked against its own encoder alone.
// Prints one line per failure and a count; exits 1 on any failure.

#include <stdio.h>

#include <string>
#include <vector>

#include "key_lines.hpp"

using namespace sshrelay;

namespace {

int failures = 0, checks = 0;

void expect(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        printf("FAIL  %s\n", what.c_str());
    }
}

// ssh-ed25519 with the key bytes 1 to 32.
const std::string kEd = "AAAAC3NzaC1lZDI1NTE5AAAAIAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8g";
// ecdsa-sha2-nistp256, nistp256, and the point 04 00 01 ... 3f.
const std::string kEc =
    "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBAABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8=";
// ssh-rsa, e 65537, and an n of 129 bytes.
const std::string kRsa =
    "AAAAB3NzaC1yc2EAAAADAQABAAAAgQABAgMEBQYHCAkKCwwNDg8QERITFBUWFxgZGhscHR4fICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9"
    "Pj9AQUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVpbXF1eX2BhYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gA==";

void accepted(const std::string& line) {
    std::string why;
    expect(checkKeyLine(line, &why), "accepted: " + line + " (refused: " + why + ")");
}

void refused(const std::string& line, const std::string& what) {
    std::string why;
    bool ok = checkKeyLine(line, &why);
    expect(!ok, "refused, " + what + ": " + line);
    expect(ok || !why.empty(), "a reason for " + what);
}

std::string blobString(const std::string& bytes) {
    std::string s;
    s.push_back((char)((bytes.size() >> 24) & 0xFF));
    s.push_back((char)((bytes.size() >> 16) & 0xFF));
    s.push_back((char)((bytes.size() >> 8) & 0xFF));
    s.push_back((char)(bytes.size() & 0xFF));
    return s + bytes;
}

std::string keysMessage(const std::vector<std::string>& lines) {
    std::string m = "{\"v\":1,\"keys\":[";
    for (size_t i = 0; i < lines.size(); ++i) m += (i ? ",\"" : "\"") + lines[i] + "\"";
    return m + "]}";
}

void testLines() {
    accepted("ssh-ed25519 " + kEd);
    accepted("ssh-ed25519 " + kEd + " laptop");
    accepted("ssh-ed25519 " + kEd + " with spaces and (punctuation) ~!@#$%^&*");
    accepted("ecdsa-sha2-nistp256 " + kEc);
    accepted("ecdsa-sha2-nistp256 " + kEc + " phone@example");
    accepted("ssh-ed25519 " + kEd + " " + std::string(kMaxComment, 'c'));

    // Options before the type: the forms authorized_keys would read as options.
    refused("command=\"/bin/true\" ssh-ed25519 " + kEd, "an option before the type");
    refused("no-pty ssh-ed25519 " + kEd, "no-pty before the type");
    refused("from=\"10.0.0.1\",no-pty ssh-ed25519 " + kEd + " x", "options before the type");
    refused(" ssh-ed25519 " + kEd, "a space before the type");
    // The type of the line and of the blob.
    refused("ecdsa-sha2-nistp256 " + kEd, "an ed25519 blob named as ecdsa");
    refused("ssh-ed25519 " + kEc, "an ecdsa blob named as ed25519");
    refused("ssh-rsa " + kRsa, "an RSA key");
    refused("ssh-ed25519 " + kRsa, "an RSA blob named as ed25519");
    refused("ssh-dss " + kEd, "an unknown type");
    refused("SSH-ED25519 " + kEd, "the type in capitals");
    // Base64.
    refused("ssh-ed25519 " + kEd.substr(0, kEd.size() - 1), "base64 cut by one character");
    refused("ssh-ed25519 " + kEd + "=", "base64 with a padding character too many");
    refused("ssh-ed25519 " + kEd.substr(0, 10) + "*" + kEd.substr(11), "a character outside base64");
    // kEc ends in "8=": the 2 bits of '8' that the padding leaves over are 0, and of '9' they are not.
    refused("ecdsa-sha2-nistp256 " + kEc.substr(0, kEc.size() - 2) + "9=", "padded base64 with bits left over");
    refused("ssh-ed25519 =" + kEd.substr(1), "padding at the start");
    // A blob cut short, or with more after the key.
    std::string ed = blobString("ssh-ed25519") + blobString(std::string(32, '\x01'));
    accepted("ssh-ed25519 " + encodeBase64(ed));
    refused("ssh-ed25519 " + encodeBase64(ed.substr(0, ed.size() - 1)), "a blob one byte short");
    refused("ssh-ed25519 " + encodeBase64(ed.substr(0, 15)), "a blob of the type's name alone");
    refused("ssh-ed25519 " + encodeBase64(ed + "x"), "a blob with a byte after the key");
    refused("ssh-ed25519 " + encodeBase64(blobString("ssh-ed25519") + blobString(std::string(31, 'k'))),
            "an ed25519 key of 31 bytes");
    refused("ssh-ed25519 " + encodeBase64(blobString("ssh-ed25519") + blobString(std::string(33, 'k'))),
            "an ed25519 key of 33 bytes");
    std::string point = "\x04" + std::string(64, 'p');
    accepted("ecdsa-sha2-nistp256 " +
             encodeBase64(blobString("ecdsa-sha2-nistp256") + blobString("nistp256") + blobString(point)));
    refused("ecdsa-sha2-nistp256 " +
                encodeBase64(blobString("ecdsa-sha2-nistp256") + blobString("nistp384") + blobString(point)),
            "an ecdsa key on another curve");
    refused("ecdsa-sha2-nistp256 " + encodeBase64(blobString("ecdsa-sha2-nistp256") + blobString("nistp256") +
                                                  blobString("\x02" + std::string(32, 'p'))),
            "a compressed point");
    refused("ssh-ed25519 " + encodeBase64("\xff\xff\xff\xff" + std::string(40, 'x')), "a blob with a huge length");
    // The shape of the line.
    refused("", "an empty line");
    refused("ssh-ed25519", "a type alone");
    refused("ssh-ed25519 ", "a type and a space");
    refused("ssh-ed25519  " + kEd, "two spaces after the type");
    refused("ssh-ed25519 " + kEd + " ", "a space at the end");
    refused("ssh-ed25519\t" + kEd, "a tab after the type");
    refused("ssh-ed25519 " + kEd + "\tlaptop", "a tab before the comment");
    refused("ssh-ed25519 " + kEd + " lap\ttop", "a tab in the comment");
    refused("ssh-ed25519 " + kEd + " laptop\n", "a line end");
    refused("ssh-ed25519 " + kEd + "\nssh-ed25519 " + kEd, "two lines in one");
    refused("ssh-ed25519 " + kEd + " lap\rtop", "a carriage return");
    refused("ssh-ed25519 " + kEd + " lap\x1btop", "an escape");
    refused("ssh-ed25519 " + kEd + " lap\x7ftop", "a delete");
    refused("ssh-ed25519 " + kEd + " caf\xc3\xa9", "a character outside ASCII");
    refused("ssh-ed25519 " + kEd + std::string("\0x", 2), "a nul byte");
    refused("ssh-ed25519 " + kEd + " " + std::string(kMaxComment + 1, 'c'), "a comment one too long");
    refused("ssh-ed25519 " + kEd + " " + std::string(10000, 'c'), "a line far too long");
    refused(std::string(kMaxKeyLine + 1, 'A'), "a line of base64 characters only");
}

void testMessage() {
    std::vector<std::string> lines, got;
    std::string why;
    lines.push_back("ssh-ed25519 " + kEd + " one");
    lines.push_back("ecdsa-sha2-nistp256 " + kEc);
    expect(readKeysMessage(keysMessage(lines), &got, &why) && got == lines, "a message of two keys: " + why);
    expect(readKeysMessage("{\"keys\":[\"ssh-ed25519 " + kEd + "\"],\"v\":1,\"later\":true}", &got, &why) &&
               got.size() == 1,
           "members other than v and keys are not looked at: " + why);
    std::vector<std::string> eight(8, "ssh-ed25519 " + kEd);
    expect(readKeysMessage(keysMessage(eight), &got, &why) && got.size() == 8, "eight keys: " + why);
    std::vector<std::string> nine(9, "ssh-ed25519 " + kEd);
    expect(!readKeysMessage(keysMessage(nine), &got, &why), "nine keys refused");
    expect(got.empty(), "nothing kept from a refused message");
    expect(!readKeysMessage(keysMessage(std::vector<std::string>()), &got, &why), "no key refused");
    std::vector<std::string> oneBad = lines;
    oneBad.push_back("no-pty ssh-ed25519 " + kEd);
    expect(!readKeysMessage(keysMessage(oneBad), &got, &why) && why.find("line 3") == 0,
           "one bad line refuses the message, and is named: " + why);
    expect(!readKeysMessage("{\"v\":2,\"keys\":[\"ssh-ed25519 " + kEd + "\"]}", &got, &why), "v 2 refused");
    expect(!readKeysMessage("{\"keys\":[\"ssh-ed25519 " + kEd + "\"]}", &got, &why), "no v refused");
    expect(!readKeysMessage("{\"v\":\"1\",\"keys\":[\"ssh-ed25519 " + kEd + "\"]}", &got, &why), "v as text refused");
    expect(!readKeysMessage("{\"v\":1}", &got, &why), "no keys refused");
    expect(!readKeysMessage("{\"v\":1,\"keys\":\"ssh-ed25519 " + kEd + "\"}", &got, &why), "keys not a list refused");
    expect(!readKeysMessage("{\"v\":1,\"keys\":[1]}", &got, &why), "a key that is not a string refused");
    expect(!readKeysMessage("{\"v\":1,\"keys\":[\"ssh-ed25519 " + kEd + "\\nssh-ed25519 " + kEd + "\"]}", &got, &why),
           "a line end written as an escape refused");
    expect(!readKeysMessage("ssh-ed25519 " + kEd, &got, &why), "not JSON refused");
    expect(!readKeysMessage("{\"v\":1,\"v\":1,\"keys\":[\"ssh-ed25519 " + kEd + "\"]}", &got, &why),
           "a member twice refused");
    expect(!readKeysMessage("[1]", &got, &why), "a list at the top refused");
    expect(describeKeyLine("ssh-ed25519 " + kEd + " a b") == "ssh-ed25519 'a b'", "described with its comment");
    expect(describeKeyLine("ssh-ed25519 " + kEd) == "ssh-ed25519 (no comment)", "described with no comment");
}

void testBase64() {
    std::string out;
    expect(decodeBase64("TWFu", &out) && out == "Man", "TWFu");
    expect(decodeBase64("TWE=", &out) && out == "Ma", "TWE=");
    expect(decodeBase64("TQ==", &out) && out == "M", "TQ==");
    expect(!decodeBase64("TR==", &out), "TR== has bits left over");
    expect(!decodeBase64("TWF", &out), "three characters");
    expect(!decodeBase64("", &out), "nothing");
    expect(!decodeBase64("====", &out), "padding alone");
    expect(!decodeBase64("T===", &out), "three padding characters");
    expect(!decodeBase64("TQ==TWFu", &out), "padding in the middle");
    std::string all;
    for (int i = 0; i < 256; ++i) all.push_back((char)i);
    for (size_t n = 0; n <= all.size(); n += 37) {
        std::string part = all.substr(0, n);
        expect(n == 0 || (decodeBase64(encodeBase64(part), &out) && out == part), "round trip");
    }
}

}  // namespace

int main() {
    testLines();
    testMessage();
    testBase64();
    printf("test_key_lines: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

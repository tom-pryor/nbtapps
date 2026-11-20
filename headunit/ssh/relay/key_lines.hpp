// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_SSH_RELAY_KEY_LINES_HPP
#define NBT_SSH_RELAY_KEY_LINES_HPP

// The keys a connection may log in with, as the phone sends them in its first message, and the rule each line is
// held to before it is written to the server's authorized_keys (ssh/README, "The relay").
//
//   { "v": 1, "keys": [ "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI... laptop", ... ] }
//
// A line is TYPE SP BASE64, or TYPE SP BASE64 SP COMMENT:
//   - TYPE is ssh-ed25519 or ecdsa-sha2-nistp256, and it is the first thing in the line, so no line can carry
//     authorized_keys options (command="...", no-pty, from=...) in front of the key;
//   - BASE64 is strict base64 (the standard alphabet, padded, no bits left over) of the key's SSH blob, whose first
//     string is the same TYPE and whose rest is that type's key and nothing else: for ssh-ed25519 a string of 32
//     bytes, for ecdsa-sha2-nistp256 the string nistp256 and an uncompressed point of 65 bytes;
//   - COMMENT, when there is one, is 1 to kMaxComment characters;
//   - every byte of the line is printable ASCII (space to tilde): no control character, tab or line end, and
//     nothing outside ASCII; the line is at most kMaxKeyLine bytes.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>

#include <string>
#include <vector>

namespace sshrelay {

const size_t kMaxKeys = 8;
const size_t kMaxComment = 128;
// The longest line the rule lets through: the longer type, its blob of 104 bytes as 140 characters of base64, and
// the longest comment, with the two spaces.
const size_t kMaxKeyLine = 19 + 1 + 140 + 1 + kMaxComment;

// Strict base64: false for any text that is not the one encoding of some bytes.
bool decodeBase64(const std::string& text, std::string* out);
std::string encodeBase64(const std::string& bytes);

// Whether a line passes the rule; *why says what is wrong with one that does not.
bool checkKeyLine(const std::string& line, std::string* why);

// The phone's first message: its lines, in order, when the message is the keys message and every line passes;
// otherwise false and *why. The message's members other than v and keys are not looked at.
bool readKeysMessage(const std::string& message, std::vector<std::string>* lines, std::string* why);

// A line that passed the rule as the log names it: its type and its comment, never the key.
std::string describeKeyLine(const std::string& line);

}  // namespace sshrelay

#endif

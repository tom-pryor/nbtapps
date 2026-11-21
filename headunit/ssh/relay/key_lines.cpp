// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The keys message and the key-line rule: see key_lines.hpp.

#include "key_lines.hpp"

#include <stdint.h>
#include <stdio.h>

#include "json.hpp"

namespace sshrelay {

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kEd25519[] = "ssh-ed25519";
const char kEcdsa[] = "ecdsa-sha2-nistp256";

int sextet(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// One SSH string of the blob at *at: four bytes of length, high byte first, and that many bytes.
bool takeString(const std::string& blob, size_t* at, std::string* out) {
    if (blob.size() - *at < 4) return false;
    uint32_t length = ((uint32_t)(unsigned char)blob[*at] << 24) | ((uint32_t)(unsigned char)blob[*at + 1] << 16) |
                      ((uint32_t)(unsigned char)blob[*at + 2] << 8) | (uint32_t)(unsigned char)blob[*at + 3];
    *at += 4;
    if (length > blob.size() - *at) return false;
    out->assign(blob, *at, length);
    *at += length;
    return true;
}

bool fail(std::string* why, const std::string& text) {
    if (why != 0) *why = text;
    return false;
}

// The blob is a key of the type, and nothing else.
bool checkBlob(const std::string& type, const std::string& blob, std::string* why) {
    size_t at = 0;
    std::string name, part;
    if (!takeString(blob, &at, &name)) return fail(why, "the key is cut short");
    if (name != type) return fail(why, "the key inside is not of the type the line names");
    if (type == kEd25519) {
        if (!takeString(blob, &at, &part)) return fail(why, "the key is cut short");
        if (part.size() != 32) return fail(why, "an ssh-ed25519 key is not 32 bytes");
    } else {
        if (!takeString(blob, &at, &part)) return fail(why, "the key is cut short");
        if (part != "nistp256") return fail(why, "the key's curve is not nistp256");
        if (!takeString(blob, &at, &part)) return fail(why, "the key is cut short");
        if (part.size() != 65 || part[0] != 4) return fail(why, "the key's point is not 65 bytes, uncompressed");
    }
    if (at != blob.size()) return fail(why, "the key has bytes after its end");
    return true;
}

}  // namespace

bool decodeBase64(const std::string& text, std::string* out) {
    out->clear();
    if (text.empty() || text.size() % 4 != 0) return false;
    size_t padding = 0;
    if (text[text.size() - 1] == '=') padding = text[text.size() - 2] == '=' ? 2 : 1;
    for (size_t i = 0; i < text.size(); i += 4) {
        bool last = i + 4 == text.size();
        uint32_t group = 0;
        for (size_t k = 0; k < 4; ++k) {
            int value = 0;
            if (last && k >= 4 - padding) {
                if (text[i + k] != '=') return false;
            } else {
                value = sextet(text[i + k]);
                if (value < 0) return false;
            }
            group = (group << 6) | (uint32_t)value;
        }
        out->push_back((char)((group >> 16) & 0xFF));
        if (!last || padding < 2) out->push_back((char)((group >> 8) & 0xFF));
        if (!last || padding < 1) out->push_back((char)(group & 0xFF));
        // The bits the padding leaves over are 0 in the one encoding of these bytes.
        if (last && padding == 2 && (group & 0xFFFF) != 0) return false;
        if (last && padding == 1 && (group & 0xFF) != 0) return false;
    }
    return true;
}

std::string encodeBase64(const std::string& bytes) {
    std::string text;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        size_t n = bytes.size() - i < 3 ? bytes.size() - i : 3;
        uint32_t group = (uint32_t)(unsigned char)bytes[i] << 16;
        if (n > 1) group |= (uint32_t)(unsigned char)bytes[i + 1] << 8;
        if (n > 2) group |= (uint32_t)(unsigned char)bytes[i + 2];
        text.push_back(kAlphabet[(group >> 18) & 63]);
        text.push_back(kAlphabet[(group >> 12) & 63]);
        text.push_back(n > 1 ? kAlphabet[(group >> 6) & 63] : '=');
        text.push_back(n > 2 ? kAlphabet[group & 63] : '=');
    }
    return text;
}

bool checkKeyLine(const std::string& line, std::string* why) {
    if (line.empty()) return fail(why, "the line is empty");
    if (line.size() > kMaxKeyLine) return fail(why, "the line is too long");
    for (size_t i = 0; i < line.size(); ++i) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c > 0x7E) return fail(why, "the line holds a character that is not printable ASCII");
    }
    size_t typeEnd = line.find(' ');
    std::string type = line.substr(0, typeEnd);
    if (type != kEd25519 && type != kEcdsa) {
        return fail(why, "the line does not start with ssh-ed25519 or ecdsa-sha2-nistp256");
    }
    if (typeEnd == std::string::npos) return fail(why, "the line has no key");
    size_t keyStart = typeEnd + 1;
    size_t keyEnd = line.find(' ', keyStart);
    std::string encoded = line.substr(keyStart, keyEnd == std::string::npos ? std::string::npos : keyEnd - keyStart);
    std::string blob;
    if (encoded.empty()) return fail(why, "the line has no key");
    if (!decodeBase64(encoded, &blob)) return fail(why, "the key is not base64");
    if (!checkBlob(type, blob, why)) return false;
    if (keyEnd != std::string::npos) {
        size_t comment = line.size() - keyEnd - 1;
        if (comment == 0) return fail(why, "the line ends in a space");
        if (comment > kMaxComment) return fail(why, "the comment is too long");
    }
    return true;
}

bool readKeysMessage(const std::string& message, std::vector<std::string>* lines, std::string* why) {
    lines->clear();
    nbt::json::Document doc;
    std::string error;
    if (!doc.parse(message, &error)) return fail(why, "the message is not JSON: " + error);
    nbt::json::Value root = doc.root();
    nbt::json::Value v = root.find("v");
    if (!v.isInteger() || v.integer() != 1) return fail(why, "v is not 1");
    nbt::json::Value keys = root.find("keys");
    if (!keys.isArray()) return fail(why, "keys is not a list");
    if (keys.size() == 0) return fail(why, "keys is empty");
    if (keys.size() > kMaxKeys) return fail(why, "keys holds more than 8 lines");
    for (size_t i = 0; i < keys.size(); ++i) {
        char where[32];
        snprintf(where, sizeof where, "line %u: ", (unsigned)(i + 1));
        if (!keys.at(i).isString()) return fail(why, std::string(where) + "not a string");
        std::string problem;
        if (!checkKeyLine(keys.at(i).string(), &problem)) return fail(why, where + problem);
        lines->push_back(keys.at(i).string());
    }
    return true;
}

std::string describeKeyLine(const std::string& line) {
    size_t typeEnd = line.find(' ');
    std::string type = line.substr(0, typeEnd);
    size_t keyEnd = typeEnd == std::string::npos ? std::string::npos : line.find(' ', typeEnd + 1);
    if (keyEnd == std::string::npos) return type + " (no comment)";
    return type + " '" + line.substr(keyEnd + 1) + "'";
}

}  // namespace sshrelay

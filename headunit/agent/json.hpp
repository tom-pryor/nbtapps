// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_JSON_HPP
#define NBT_AGENT_JSON_HPP

// Parser for the strict JSON profile that manifest.json and the protocol's messages share (plan document, "Message
// reference", Common rules). A text outside the profile is refused whole:
//
//   - UTF-8 with no byte-order mark, and one object at the top level
//   - no object has the same key twice (compared after escapes are decoded)
//   - numbers are integers: no fraction, no exponent, at most 2^53 - 1 in size
//   - strings are valid UTF-8, with no unescaped control character and no unpaired surrogate escape
//   - objects and arrays nest at most 16 levels deep
//
// The profile exists because more than one parser reads these texts (this one, tools/nbtpkg.py, the Android app),
// and a text that two of them understand differently must not get through any of them. All of them are tested
// against spec/json-profile.vectors.
//
// The caller bounds the input: a message is at most one frame (64 KiB) and a manifest at most 1 MiB. The tree takes
// up to roughly 100 times the input in memory in the worst case, so nothing larger should be passed in.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace nbt {
namespace json {

const unsigned kMaxDepth = 16;
const int64_t kMaxInteger = 9007199254740991LL;   // 2^53 - 1

enum Type { kNull, kBool, kInteger, kString, kArray, kObject };

class Document;

// A value inside a Document: a cheap handle, valid for as long as the Document lives and is not parsed into again.
// A default-made Value is "absent": it has no type, and every isX() is false. find() returns one for a missing key,
// so a lookup and a type check chain without a null test: `if (!root.find("id").isString()) ...`.
class Value {
public:
    Value() : doc_(0), index_(0) {}

    bool present() const { return doc_ != 0; }
    bool isNull() const { return is(kNull); }
    bool isBool() const { return is(kBool); }
    bool isInteger() const { return is(kInteger); }
    bool isString() const { return is(kString); }
    bool isArray() const { return is(kArray); }
    bool isObject() const { return is(kObject); }

    // Each of these needs the matching isX() to be true.
    bool boolean() const;
    int64_t integer() const;
    const std::string& string() const;

    // Arrays and objects: the number of elements or members, and the one at position i, in the order of the text.
    size_t size() const;
    Value at(size_t i) const;
    // Objects: the name of member i, and the member with a given name (absent if there is none).
    const std::string& key(size_t i) const;
    Value find(const std::string& name) const;

private:
    friend class Document;
    Value(const Document* doc, uint32_t index) : doc_(doc), index_(index) {}
    bool is(Type type) const;

    const Document* doc_;
    uint32_t index_;
};

class Document {
public:
    // Parses `len` bytes under the profile. Returns false and sets *error (a short reason with the byte offset) for a
    // text outside it; the document is then empty. `error` may be 0.
    bool parse(const void* data, size_t len, std::string* error);
    bool parse(const std::string& text, std::string* error) { return parse(text.data(), text.size(), error); }

    // The top-level object; absent before a successful parse.
    Value root() const { return nodes_.empty() ? Value() : Value(this, 0); }

private:
    friend class Value;
    friend class Parser;
    struct Node {
        Type type;
        int64_t number;               // kInteger: the value; kBool: 0 or 1
        std::string text;             // kString: the decoded value
        std::string key;              // the member name, when the parent is an object
        std::vector<uint32_t> kids;   // kArray, kObject: the children, in order
        Node() : type(kNull), number(0) {}
    };
    std::vector<Node> nodes_;
};

}  // namespace json
}  // namespace nbt

#endif

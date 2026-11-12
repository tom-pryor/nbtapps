// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The strict JSON profile: see json.hpp. A recursive-descent parser over the raw bytes. Nesting is capped at 16
// levels, so the recursion is too.

#include "json.hpp"

#include <stdio.h>

#include <set>

namespace nbt {
namespace json {

// ---- Value ------------------------------------------------------------------------------------------------------

bool Value::is(Type type) const { return doc_ != 0 && doc_->nodes_[index_].type == type; }
bool Value::boolean() const { return doc_->nodes_[index_].number != 0; }
int64_t Value::integer() const { return doc_->nodes_[index_].number; }
const std::string& Value::string() const { return doc_->nodes_[index_].text; }
size_t Value::size() const { return doc_ != 0 ? doc_->nodes_[index_].kids.size() : 0; }
Value Value::at(size_t i) const { return Value(doc_, doc_->nodes_[index_].kids[i]); }
const std::string& Value::key(size_t i) const { return doc_->nodes_[doc_->nodes_[index_].kids[i]].key; }

Value Value::find(const std::string& name) const {
    if (!isObject()) return Value();
    const std::vector<uint32_t>& kids = doc_->nodes_[index_].kids;
    for (size_t i = 0; i < kids.size(); ++i) {
        if (doc_->nodes_[kids[i]].key == name) return Value(doc_, kids[i]);
    }
    return Value();
}

// ---- Parser -----------------------------------------------------------------------------------------------------

class Parser {
public:
    Parser(const unsigned char* data, size_t len, std::vector<Document::Node>* nodes)
        : begin_(data), p_(data), end_(data + len), nodes_(nodes) {}

    bool run() {
        if (end_ - p_ >= 3 && p_[0] == 0xEF && p_[1] == 0xBB && p_[2] == 0xBF) return fail("byte-order mark");
        skipSpace();
        if (p_ == end_ || *p_ != '{') return fail("the top level is not an object");
        uint32_t root;
        if (!value(&root, 1)) return false;
        skipSpace();
        if (p_ != end_) return fail("text after the end of the object");
        return true;
    }

    const std::string& error() const { return error_; }

private:
    bool fail(const char* what) {
        if (error_.empty()) {
            char at[32];
            snprintf(at, sizeof at, " at byte %lu", (unsigned long)(p_ - begin_));
            error_ = std::string(what) + at;
        }
        return false;
    }

    // JSON whitespace is these four characters and nothing else.
    void skipSpace() {
        while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }

    // Nodes are addressed by index throughout: the vector moves as it grows.
    uint32_t add(Type type) {
        nodes_->push_back(Document::Node());
        nodes_->back().type = type;
        return (uint32_t)(nodes_->size() - 1);
    }

    bool value(uint32_t* index, unsigned depth) {
        if (p_ == end_) return fail("the text ends where a value should be");
        switch (*p_) {
        case '{':
            if (depth > kMaxDepth) return fail("nested too deeply");
            *index = add(kObject);
            return object(*index, depth);
        case '[':
            if (depth > kMaxDepth) return fail("nested too deeply");
            *index = add(kArray);
            return array(*index, depth);
        case '"': {
            std::string text;
            if (!string(&text)) return false;
            *index = add(kString);
            (*nodes_)[*index].text.swap(text);
            return true;
        }
        case 't':
            if (!word("true")) return false;
            *index = add(kBool);
            (*nodes_)[*index].number = 1;
            return true;
        case 'f':
            if (!word("false")) return false;
            *index = add(kBool);
            return true;
        case 'n':
            if (!word("null")) return false;
            *index = add(kNull);
            return true;
        default: {
            int64_t n = 0;
            if (!number(&n)) return false;
            *index = add(kInteger);
            (*nodes_)[*index].number = n;
            return true;
        }
        }
    }

    bool object(uint32_t self, unsigned depth) {
        ++p_;   // {
        skipSpace();
        if (p_ != end_ && *p_ == '}') {
            ++p_;
            return true;
        }
        std::set<std::string> seen;
        for (;;) {
            if (p_ == end_ || *p_ != '"') return fail("a member name was expected");
            std::string name;
            if (!string(&name)) return false;
            if (!seen.insert(name).second) return fail("the same key twice in one object");
            skipSpace();
            if (p_ == end_ || *p_ != ':') return fail("a colon was expected");
            ++p_;
            skipSpace();
            uint32_t child;
            if (!value(&child, depth + 1)) return false;
            (*nodes_)[child].key.swap(name);
            (*nodes_)[self].kids.push_back(child);
            skipSpace();
            if (p_ != end_ && *p_ == ',') {
                ++p_;
                skipSpace();
                continue;
            }
            if (p_ != end_ && *p_ == '}') {
                ++p_;
                return true;
            }
            return fail("a comma or a closing brace was expected");
        }
    }

    bool array(uint32_t self, unsigned depth) {
        ++p_;   // [
        skipSpace();
        if (p_ != end_ && *p_ == ']') {
            ++p_;
            return true;
        }
        for (;;) {
            uint32_t child;
            if (!value(&child, depth + 1)) return false;
            (*nodes_)[self].kids.push_back(child);
            skipSpace();
            if (p_ != end_ && *p_ == ',') {
                ++p_;
                skipSpace();
                continue;
            }
            if (p_ != end_ && *p_ == ']') {
                ++p_;
                return true;
            }
            return fail("a comma or a closing bracket was expected");
        }
    }

    // What follows a literal or a number is checked by the caller, which wants a comma or a closing bracket: that is
    // what refuses `truefalse`, `01` and `1_000`.
    bool word(const char* text) {
        for (const char* c = text; *c != '\0'; ++c, ++p_) {
            if (p_ == end_ || *p_ != (unsigned char)*c) return fail("an unknown word where a value should be");
        }
        return true;
    }

    bool number(int64_t* out) {
        bool negative = false;
        if (*p_ == '-') {
            negative = true;
            ++p_;
        }
        if (p_ == end_ || *p_ < '0' || *p_ > '9') return fail("a value was expected");
        uint64_t n = 0;
        if (*p_ == '0') {
            ++p_;   // a leading zero stands alone; a digit after it is refused by the caller
        } else {
            // 2^53 - 1 has 16 digits, so 16 digits cannot overflow and a 17th is already out of range.
            int digits = 0;
            while (p_ != end_ && *p_ >= '0' && *p_ <= '9') {
                if (++digits > 16) return fail("a number out of range");
                n = n * 10 + (uint64_t)(*p_ - '0');
                ++p_;
            }
            if (n > (uint64_t)kMaxInteger) return fail("a number out of range");
        }
        if (p_ != end_ && (*p_ == '.' || *p_ == 'e' || *p_ == 'E')) return fail("a number that is not an integer");
        *out = negative ? -(int64_t)n : (int64_t)n;
        return true;
    }

    bool hex4(unsigned* out) {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i, ++p_) {
            if (p_ == end_) return fail("an escape cut short");
            unsigned char c = *p_;
            unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return fail("an escape that is not four hex digits");
            v = v * 16 + digit;
        }
        *out = v;
        return true;
    }

    static void appendUtf8(std::string* out, unsigned cp) {
        if (cp < 0x80) {
            *out += (char)cp;
        } else if (cp < 0x800) {
            *out += (char)(0xC0 | (cp >> 6));
            *out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *out += (char)(0xE0 | (cp >> 12));
            *out += (char)(0x80 | ((cp >> 6) & 0x3F));
            *out += (char)(0x80 | (cp & 0x3F));
        } else {
            *out += (char)(0xF0 | (cp >> 18));
            *out += (char)(0x80 | ((cp >> 12) & 0x3F));
            *out += (char)(0x80 | ((cp >> 6) & 0x3F));
            *out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool escape(std::string* out) {
        ++p_;   // the backslash
        if (p_ == end_) return fail("an escape cut short");
        unsigned char c = *p_++;
        switch (c) {
        case '"': *out += '"'; return true;
        case '\\': *out += '\\'; return true;
        case '/': *out += '/'; return true;
        case 'b': *out += '\b'; return true;
        case 'f': *out += '\f'; return true;
        case 'n': *out += '\n'; return true;
        case 'r': *out += '\r'; return true;
        case 't': *out += '\t'; return true;
        case 'u': break;
        default: --p_; return fail("an unknown escape");
        }
        unsigned cp = 0;
        if (!hex4(&cp)) return false;
        if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("a low surrogate with no high surrogate before it");
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // A high surrogate counts only together with the low surrogate escape right after it.
            if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') return fail("a high surrogate with no low surrogate after it");
            p_ += 2;
            unsigned low = 0;
            if (!hex4(&low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return fail("a high surrogate with no low surrogate after it");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        appendUtf8(out, cp);
        return true;
    }

    // One UTF-8 sequence of two to four bytes, in its shortest form, not a surrogate and not above U+10FFFF
    // (Unicode, table 3-7). The lead byte fixes the range of the second.
    bool multibyte(std::string* out) {
        unsigned char lead = *p_;
        int more;
        unsigned char low = 0x80, high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            more = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            more = 2;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            more = 3;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        } else {
            return fail("a byte that is not valid UTF-8");
        }
        if (end_ - p_ < more + 1) return fail("a UTF-8 sequence cut short");
        if (p_[1] < low || p_[1] > high) return fail("a byte that is not valid UTF-8");
        for (int i = 2; i <= more; ++i) {
            if (p_[i] < 0x80 || p_[i] > 0xBF) return fail("a byte that is not valid UTF-8");
        }
        out->append((const char*)p_, (size_t)more + 1);
        p_ += more + 1;
        return true;
    }

    bool string(std::string* out) {
        ++p_;   // the opening quote
        for (;;) {
            if (p_ == end_) return fail("a string with no closing quote");
            unsigned char c = *p_;
            if (c == '"') {
                ++p_;
                return true;
            }
            if (c < 0x20) return fail("a control character in a string");
            if (c == '\\') {
                if (!escape(out)) return false;
            } else if (c < 0x80) {
                *out += (char)c;
                ++p_;
            } else if (!multibyte(out)) {
                return false;
            }
        }
    }

    const unsigned char* begin_;
    const unsigned char* p_;
    const unsigned char* end_;
    std::vector<Document::Node>* nodes_;
    std::string error_;
};

bool Document::parse(const void* data, size_t len, std::string* error) {
    nodes_.clear();
    Parser parser((const unsigned char*)data, len, &nodes_);
    if (parser.run()) return true;
    nodes_.clear();
    if (error != 0) *error = parser.error();
    return false;
}

}  // namespace json
}  // namespace nbt

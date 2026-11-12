// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/json.cpp: every line of spec/json-profile.vectors (the cases tools/nbtpkg.py is held to as
// well), then the tree a parse gives: types, order, lookups, decoded escapes and error positions.
//
// usage: test_json [path to json-profile.vectors]   (default spec/json-profile.vectors, as `make` runs it)

#include "../../agent/json.hpp"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

using namespace nbt::json;

static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// "ok <text>" or "bad <text>", %HH for a byte, an optional note after two spaces and a #.
static int runVectors(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == 0) {
        fprintf(stderr, "test_json: cannot open %s\n", path);
        exit(1);
    }
    int cases = 0, lineNumber = 0;
    std::string line;
    for (;;) {
        line.clear();
        int c;
        while ((c = fgetc(f)) != EOF && c != '\n') line += (char)c;
        if (c == EOF && line.empty()) break;
        ++lineNumber;
        if (line.empty() || line[0] == '#') continue;
        size_t note = line.find("  #");
        if (note != std::string::npos) line.erase(note);
        size_t space = line.find(' ');
        std::string verdict = line.substr(0, space);
        std::string text = space == std::string::npos ? "" : line.substr(space + 1);
        assert(verdict == "ok" || verdict == "bad");
        std::string input;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%') {
                assert(i + 2 < text.size() && hexDigit(text[i + 1]) >= 0 && hexDigit(text[i + 2]) >= 0);
                input += (char)(hexDigit(text[i + 1]) * 16 + hexDigit(text[i + 2]));
                i += 2;
            } else {
                input += text[i];
            }
        }
        Document doc;
        std::string error;
        bool accepted = doc.parse(input, &error);
        if (accepted != (verdict == "ok")) {
            fprintf(stderr, "test_json: %s line %d: %s%s%s\n", path, lineNumber, accepted ? "accepted" : "refused",
                    accepted ? "" : ": ", error.c_str());
            exit(1);
        }
        // A refusal says why and leaves nothing behind; an acceptance has an object at the root.
        assert(accepted ? doc.root().isObject() : (!error.empty() && !doc.root().present()));
        ++cases;
    }
    fclose(f);
    return cases;
}

int main(int argc, char** argv) {
    // 1. The shared conformance cases.
    int cases = runVectors(argc > 1 ? argv[1] : "spec/json-profile.vectors");
    assert(cases >= 100);

    // 2. The tree: types, values and the order of the text.
    Document doc;
    std::string error;
    assert(doc.parse("{\"id\":\"org.nbtapp.hello\",\"size\":48213,\"neg\":-7,\"on\":true,\"off\":false,\"none\":null,"
                     "\"list\":[1,\"two\",[3],{\"four\":4}],\"empty\":{},\"\":0}",
                     &error));
    Value root = doc.root();
    assert(root.present() && root.isObject() && !root.isArray() && root.size() == 9);
    assert(root.key(0) == "id" && root.key(1) == "size" && root.key(8) == "");
    assert(root.find("id").isString() && root.find("id").string() == "org.nbtapp.hello");
    assert(root.find("size").isInteger() && root.find("size").integer() == 48213);
    assert(root.find("neg").integer() == -7);
    assert(root.find("on").isBool() && root.find("on").boolean() && !root.find("off").boolean());
    assert(root.find("none").isNull() && root.find("none").present());
    assert(root.find("").isInteger() && root.find("").integer() == 0);
    assert(root.at(0).string() == "org.nbtapp.hello" && root.at(1).integer() == 48213);

    Value list = root.find("list");
    assert(list.isArray() && list.size() == 4);
    assert(list.at(0).integer() == 1 && list.at(1).string() == "two");
    assert(list.at(2).isArray() && list.at(2).size() == 1 && list.at(2).at(0).integer() == 3);
    assert(list.at(3).isObject() && list.at(3).find("four").integer() == 4);
    assert(root.find("empty").isObject() && root.find("empty").size() == 0);

    // A missing key gives an absent value: no type, size 0, and lookups on it stay absent.
    Value missing = root.find("missing");
    assert(!missing.present() && !missing.isNull() && !missing.isObject() && !missing.isString());
    assert(missing.size() == 0 && !missing.find("x").present());
    assert(!list.find("x").present() && !root.find("id").find("x").present());
    assert(!root.find("ID").present());
    // The type checks do not blur: a number is not a bool, a string is not a number.
    assert(!root.find("size").isBool() && !root.find("on").isInteger() && !root.find("id").isInteger());

    // 3. Numbers at the edge of the range.
    assert(doc.parse("{\"a\":9007199254740991,\"b\":-9007199254740991,\"c\":-0}", &error));
    assert(doc.root().find("a").integer() == kMaxInteger && doc.root().find("b").integer() == -kMaxInteger);
    assert(doc.root().find("c").integer() == 0);

    // 4. Strings come back decoded: escapes, a surrogate pair as four UTF-8 bytes, and an escaped NUL kept.
    assert(doc.parse("{\"a\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\",\"b\":\"\\u0041\\u00e9\\u20ac\\ud83d\\ude00\","
                     "\"c\":\"x\\u0000y\",\"d\\u0041\":1}",
                     &error));
    assert(doc.root().find("a").string() == "\"\\/\b\f\n\r\t");
    assert(doc.root().find("b").string() == "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
    assert(doc.root().find("c").string() == std::string("x\0y", 3));
    assert(doc.root().find("dA").integer() == 1);
    // Raw UTF-8 passes through unchanged.
    assert(doc.parse("{\"a\":\"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\"}", &error));
    assert(doc.root().find("a").string() == "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");

    // 5. A failed parse empties the document, names the reason and the place, and the document can be used again.
    assert(doc.parse("{\"a\":1}", &error) && doc.root().present());
    assert(!doc.parse("{\"a\":1,\"a\":2}", &error));
    assert(!doc.root().present() && error == "the same key twice in one object at byte 10");
    assert(!doc.parse("{\"a\":1.5}", &error) && error == "a number that is not an integer at byte 6");
    assert(!doc.parse("[]", &error) && error == "the top level is not an object at byte 0");
    assert(!doc.parse("", 0, &error) && !doc.parse("{\"a\":1}", 6, &error));
    assert(!doc.parse("[]", 0));   // a caller that wants no reason passes 0
    assert(doc.parse("{\"a\":2}", &error) && doc.root().find("a").integer() == 2);
    // The length is what bounds the text, not a NUL.
    const char twoTexts[] = "{\"a\":1}{\"b\":2}";
    assert(doc.parse(twoTexts, 7, &error) && doc.root().size() == 1);

    // 6. A wide object: duplicates are still found, and a lookup finds the last member.
    std::string wide = "{";
    for (int i = 0; i < 2000; ++i) {
        char member[32];
        snprintf(member, sizeof member, "%s\"k%d\":%d", i == 0 ? "" : ",", i, i);
        wide += member;
    }
    assert(doc.parse(wide + "}", &error) && doc.root().size() == 2000 && doc.root().find("k1999").integer() == 1999);
    assert(!doc.parse(wide + ",\"k1234\":0}", &error));

    printf("test_json: ok (%d shared cases)\n", cases);
    return 0;
}

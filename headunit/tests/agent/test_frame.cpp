// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/frame.cpp, and the keeper of spec/protocol/frames.vectors: the frames every implementation
// of the protocol must build and refuse alike (the Android app's codec is tested against the same file).
//
// usage: test_frame [spec/protocol/frames.vectors]
//        test_frame --write FILE      writes the vectors file anew, from the cases in this test

#include "../../agent/frame.hpp"

#include <string.h>

#include <vector>

#include "../../agent/inflate.hpp"
#include "test_util.hpp"

using namespace nbt;

static std::string hex(const std::string& bytes) { return toHex((const uint8_t*)bytes.data(), bytes.size()); }

static std::string unhex(const std::string& text) {
    std::string out(text.size() / 2, '\0');
    CHECK_IN(text, text.size() % 2 == 0 && (text.empty() || fromHex(text, (uint8_t*)&out[0], out.size())));
    return out;
}

// A frame put together by hand, with any header at all, so that broken ones can be made.
static std::string rawFrame(unsigned type, unsigned flags, unsigned stream, uint32_t length, const std::string& payload,
                            bool goodCheck = true) {
    std::string out;
    out += (char)type;
    out += (char)flags;
    out += (char)(stream & 0xFF);
    out += (char)(stream >> 8);
    for (int i = 0; i < 4; ++i) out += (char)((length >> (8 * i)) & 0xFF);
    out += payload;
    uint32_t crc = crc32(0, out.data(), out.size());
    if (!goodCheck) crc ^= 1;
    for (int i = 0; i < 4; ++i) out += (char)((crc >> (8 * i)) & 0xFF);
    return out;
}

struct GoodCase {
    const char* what;
    uint8_t type, flags;
    uint16_t stream;
    std::string payload;
};

struct BadCase {
    const char* what;
    std::string bytes;
};

static std::vector<GoodCase> goodCases() {
    std::vector<GoodCase> cases;
    GoodCase c;
    c.what = "a request";
    c.type = kFrameMsg;
    c.flags = 0;
    c.stream = 0;
    c.payload = "{\"id\":1,\"method\":\"info.get\",\"params\":{}}";
    cases.push_back(c);
    c.what = "a response with text outside ASCII";
    c.payload = "{\"id\":1,\"result\":{\"name\":\"Gr\xC3\xBC\xC3\x9F" "e \xE2\x9C\x93\"}}";
    cases.push_back(c);
    c.what = "a MSG frame with an empty payload: a good frame, though not a message";
    c.payload = "";
    cases.push_back(c);
    c.what = "a DATA frame in the middle of stream 5";
    c.type = kFrameData;
    c.stream = 5;
    c.payload = std::string("\x00\x01\x02\xFF\xFE", 5);
    cases.push_back(c);
    c.what = "the last DATA frame of stream 5";
    c.flags = kFlagLast;
    c.payload = "end";
    cases.push_back(c);
    c.what = "a last DATA frame with no bytes, on the highest stream number";
    c.stream = 65535;
    c.payload = "";
    cases.push_back(c);
    c.what = "a DATA frame of 300 bytes, so the length needs its second byte";
    c.flags = 0;
    c.stream = 258;
    c.payload.clear();
    for (int i = 0; i < 300; ++i) c.payload += (char)(i * 7);
    cases.push_back(c);
    return cases;
}

static std::vector<BadCase> badCases() {
    std::vector<BadCase> cases;
    BadCase c;
    c.what = "frame type 0";
    c.bytes = rawFrame(0, 0, 0, 2, "{}");
    cases.push_back(c);
    c.what = "frame type 3";
    c.bytes = rawFrame(3, 0, 0, 2, "{}");
    cases.push_back(c);
    c.what = "a MSG frame with the reserved compression flag";
    c.bytes = rawFrame(1, 1, 0, 2, "{}");
    cases.push_back(c);
    c.what = "a MSG frame with the last-frame flag";
    c.bytes = rawFrame(1, 2, 0, 2, "{}");
    cases.push_back(c);
    c.what = "a MSG frame with a stream number";
    c.bytes = rawFrame(1, 0, 1, 2, "{}");
    cases.push_back(c);
    c.what = "a DATA frame on stream 0";
    c.bytes = rawFrame(2, 0, 0, 1, "x");
    cases.push_back(c);
    c.what = "a DATA frame with the reserved compression flag";
    c.bytes = rawFrame(2, 3, 7, 1, "x");
    cases.push_back(c);
    c.what = "a DATA frame with an unknown flag";
    c.bytes = rawFrame(2, 4, 7, 1, "x");
    cases.push_back(c);
    c.what = "a wrong check value";
    c.bytes = rawFrame(1, 0, 0, 2, "{}", false);
    cases.push_back(c);
    c.what = "one payload byte changed after the check value was made";
    c.bytes = rawFrame(2, 0, 7, 4, "abcd");
    c.bytes[9] = 'X';
    cases.push_back(c);
    c.what = "a length of 65,537: refused from the header alone, with no payload behind it";
    c.bytes = rawFrame(1, 0, 0, 65537, "").substr(0, kFrameHeaderBytes);
    cases.push_back(c);
    c.what = "a length of 4 GiB less one";
    c.bytes = rawFrame(2, 0, 1, 0xFFFFFFFFu, "").substr(0, kFrameHeaderBytes);
    cases.push_back(c);
    return cases;
}

static FrameDecoder::Result decodeAll(const std::string& bytes, Frame* frame, std::string* why) {
    FrameDecoder decoder;
    decoder.feed(bytes.data(), bytes.size());
    return decoder.next(frame, why);
}

static void testCases() {
    std::vector<GoodCase> good = goodCases();
    for (size_t i = 0; i < good.size(); ++i) {
        const GoodCase& c = good[i];
        std::string encoded;
        encodeFrame(c.type, c.flags, c.stream, c.payload.data(), c.payload.size(), &encoded);
        CHECK_IN(c.what, encoded == rawFrame(c.type, c.flags, c.stream, (uint32_t)c.payload.size(), c.payload));
        CHECK_IN(c.what, encoded.size() == kFrameHeaderBytes + c.payload.size() + kFrameCheckBytes);
        Frame frame;
        std::string why;
        CHECK_IN(c.what, decodeAll(encoded, &frame, &why) == FrameDecoder::kFrame);
        CHECK_IN(c.what, frame.type == c.type && frame.flags == c.flags && frame.stream == c.stream && frame.payload == c.payload);
        CHECK_IN(c.what, frame.last() == ((c.flags & kFlagLast) != 0));
        // Every cut-off form of it is "not yet", never a frame and never an error.
        for (size_t cut = 0; cut < encoded.size(); ++cut) {
            CHECK_IN(c.what, decodeAll(encoded.substr(0, cut), &frame, &why) == FrameDecoder::kNeedMore);
        }
    }
    std::vector<BadCase> bad = badCases();
    for (size_t i = 0; i < bad.size(); ++i) {
        Frame frame;
        std::string why;
        CHECK_IN(bad[i].what, decodeAll(bad[i].bytes, &frame, &why) == FrameDecoder::kError);
        CHECK_IN(bad[i].what, !why.empty());
    }
}

static void testStream() {
    // Several frames in one piece, then the same bytes one at a time: a link's pieces mean nothing.
    std::vector<GoodCase> good = goodCases();
    std::string all;
    for (size_t i = 0; i < good.size(); ++i) {
        encodeFrame(good[i].type, good[i].flags, good[i].stream, good[i].payload.data(), good[i].payload.size(), &all);
    }
    for (int pass = 0; pass < 2; ++pass) {
        FrameDecoder decoder;
        size_t got = 0;
        Frame frame;
        std::string why;
        if (pass == 0) decoder.feed(all.data(), all.size());
        for (size_t at = 0; pass == 0 ? at < 1 : at < all.size(); ++at) {
            if (pass == 1) decoder.feed(all.data() + at, 1);
            FrameDecoder::Result r;
            while ((r = decoder.next(&frame, &why)) == FrameDecoder::kFrame) {
                CHECK(got < good.size() && frame.payload == good[got].payload && frame.stream == good[got].stream);
                ++got;
            }
            CHECK(r == FrameDecoder::kNeedMore);
        }
        CHECK(got == good.size() && decoder.buffered() == 0);
    }

    // The limit that `hello` sets counts from then on, in both directions of the comparison.
    std::string big(2000, 'x'), encoded;
    encodeFrame(kFrameData, 0, 1, big.data(), big.size(), &encoded);
    FrameDecoder decoder;
    decoder.setMaxPayload(1999);
    decoder.feed(encoded.data(), encoded.size());
    Frame frame;
    std::string why;
    CHECK(decoder.next(&frame, &why) == FrameDecoder::kError);
    // After an error the decoder stays failed, whatever is fed.
    decoder.feed(encoded.data(), encoded.size());
    CHECK(decoder.next(&frame, &why) == FrameDecoder::kError);
    FrameDecoder exact;
    exact.setMaxPayload(2000);
    exact.feed(encoded.data(), encoded.size());
    CHECK(exact.next(&frame, &why) == FrameDecoder::kFrame && frame.payload == big);

    // The largest frame there is.
    std::string largest(kMaxFramePayload, 'y');
    encoded.clear();
    encodeFrame(kFrameData, kFlagLast, 9, largest.data(), largest.size(), &encoded);
    FrameDecoder full;
    full.feed(encoded.data(), encoded.size());
    CHECK(full.next(&frame, &why) == FrameDecoder::kFrame && frame.payload == largest && frame.last());

    CHECK(memcmp(kLinkMagic, "NBTL", 4) == 0);
}

// ---- the vectors file ---------------------------------------------------------------------------------------------

static std::string vectorsText() {
    std::string out =
        "# Frames of the phone and agent protocol (see the plan document, \"Framing\"). Every implementation must agree\n"
        "# with every line: the agent's agent/frame.cpp (tests/agent/test_frame.cpp, which writes this file) and the\n"
        "# Android app's codec. tests/test_protocol_vectors.py checks the file with a third, independent reading.\n"
        "#\n"
        "#   magic HEX                                the bytes the side that dialled sends before its first frame\n"
        "#   frame TYPE FLAGS STREAM PAYLOAD FRAME    a good frame: its fields in decimal, its payload and the whole\n"
        "#                                            frame in hex; a payload of no bytes is written -\n"
        "#   bad BYTES                                bytes a decoder must refuse, and close the link over\n"
        "#\n"
        "# A line that starts with # says what the next line is. Cut off anywhere, a good frame is not an error: the\n"
        "# decoder waits for more.\n\n";
    out += "magic " + toHex(kLinkMagic, kLinkMagicBytes) + "\n\n";
    std::vector<GoodCase> good = goodCases();
    for (size_t i = 0; i < good.size(); ++i) {
        const GoodCase& c = good[i];
        std::string encoded;
        encodeFrame(c.type, c.flags, c.stream, c.payload.data(), c.payload.size(), &encoded);
        char fields[64];
        snprintf(fields, sizeof fields, "frame %u %u %u ", (unsigned)c.type, (unsigned)c.flags, (unsigned)c.stream);
        out += std::string("# ") + c.what + "\n" + fields + (c.payload.empty() ? "-" : hex(c.payload)) + " " + hex(encoded) + "\n";
    }
    out += "\n";
    std::vector<BadCase> bad = badCases();
    for (size_t i = 0; i < bad.size(); ++i) out += std::string("# ") + bad[i].what + "\nbad " + hex(bad[i].bytes) + "\n";
    return out;
}

static void testVectorsFile(const std::string& path) {
    std::string text = testutil::readFile(path);
    if (text != vectorsText()) {
        fprintf(stderr, "%s is not what this test would write. If the cases changed on purpose, run\n  test_frame --write %s\n", path.c_str(), path.c_str());
        exit(1);
    }
    // And read back as another implementation would read it.
    size_t at = 0, frames = 0, bad = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.compare(0, 6, "frame ") == 0) {
            unsigned type = 0, flags = 0, stream = 0;
            char payload[1024], whole[2048];
            CHECK_IN(line, sscanf(line.c_str(), "frame %u %u %u %1023s %2047s", &type, &flags, &stream, payload, whole) == 5);
            Frame frame;
            std::string why;
            CHECK_IN(line, decodeAll(unhex(whole), &frame, &why) == FrameDecoder::kFrame);
            CHECK_IN(line, frame.type == type && frame.flags == flags && frame.stream == stream);
            CHECK_IN(line, frame.payload == (strcmp(payload, "-") == 0 ? std::string() : unhex(payload)));
            ++frames;
        } else if (line.compare(0, 4, "bad ") == 0) {
            Frame frame;
            std::string why;
            CHECK_IN(line, decodeAll(unhex(line.substr(4)), &frame, &why) == FrameDecoder::kError);
            ++bad;
        }
    }
    CHECK(frames == goodCases().size() && bad == badCases().size());
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--write") == 0) {
        FILE* f = fopen(argv[2], "wb");
        CHECK_IN(argv[2], f != 0);
        std::string text = vectorsText();
        CHECK(fwrite(text.data(), 1, text.size(), f) == text.size());
        fclose(f);
        printf("wrote %s\n", argv[2]);
        return 0;
    }
    testCases();
    testStream();
    testVectorsFile(argc > 1 ? argv[1] : "spec/protocol/frames.vectors");
    printf("test_frame: ok\n");
    return 0;
}

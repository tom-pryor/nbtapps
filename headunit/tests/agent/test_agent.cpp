// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for the agent's protocol side: agent/agent.cpp, peer.cpp, planner.cpp and job.cpp. An Agent runs in
// this process on a HostPlatform folder, and "phones" talk to it over socket pairs. A phone here builds and reads
// its frames with code of its own, not with the agent's, so that the two can disagree.
//
// One thread: a phone writes, the agent takes a turn, the phone reads, until nothing moves any more. Time is a
// clock the test sets, so nothing here depends on how fast the machine is.
//
// The agent's own update runs here as several agents, one after the other, on one folder: the one that places
// the new version, the new version on trial, and the old one again when nobody confirmed the new.
//
// It also keeps spec/protocol/session.vectors, a whole conversation frame by frame, for the Android app's tests.
//
// usage: test_agent [testdata folder] [work folder] [session vectors file]
//        test_agent --write-session FILE [testdata folder] [work folder]

#include "../../agent/agent.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include <map>
#include <set>
#include <vector>

#include "../../agent/ed25519.hpp"
#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "../../agent/json.hpp"
#include "../../agent/sha256.hpp"
#include "../../agent/utf8.hpp"
#include "test_util.hpp"

using namespace nbt;

// ---- the test packages --------------------------------------------------------------------------------------------

struct Pkg {
    std::string id, version, sha256, manifestSha256, signature, bytes, manifest;
    uint64_t size;
};

static std::string gData, gWork;
static std::map<std::string, Pkg> gPkgs;   // "org.nbtapp.hello-1.2.0" -> the package
static TrustedKey gPackageKey, gOtherKey;

static const Pkg& pkg(const std::string& name) {
    CHECK_IN(name, gPkgs.count(name) != 0);
    return gPkgs[name];
}

static std::string num(long long value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    return text;
}

static std::string hashOf(const std::string& data) {
    uint8_t digest[kSha256Bytes];
    sha256(data.data(), data.size(), digest);
    return toHex(digest, sizeof digest);
}

static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

static std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(' ', at);
        if (end == std::string::npos) end = text.size();
        if (end > at) out.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

static void loadPackages() {
    std::string text = testutil::readFile(gData + "/vectors.json"), why;
    json::Document doc;
    CHECK_IN(why, doc.parse(text, &why));
    json::Value keys = doc.root().find("keys"), good = doc.root().find("good");
    gPackageKey = testutil::key(keys.find("package").string(), "test package key", true);
    gOtherKey = testutil::key(keys.find("other").string(), "another source", false);
    std::vector<TrustedKey> both;
    both.push_back(gPackageKey);
    both.push_back(gOtherKey);
    for (size_t i = 0; i < good.size(); ++i) {
        json::Value g = good.at(i);
        std::string file = g.find("file").string();
        std::string name = file.substr(5, file.size() - 5 - 7);   // "good/" ... ".nbtpkg"
        Pkg p;
        p.id = g.find("id").string();
        p.version = g.find("version").string();
        p.sha256 = g.find("sha256").string();
        p.manifestSha256 = g.find("manifestSha256").string();
        p.signature = g.find("signature").string();
        p.size = (uint64_t)g.find("size").integer();
        p.bytes = testutil::readFile(gData + "/" + file);
        MemorySource source(p.bytes.data(), p.bytes.size());
        Package package;
        Error error;
        CHECK_IN(name + ": " + error.message, package.open(&source, both, &error));
        p.manifest = package.manifestBytes();
        CHECK_IN(name, hashOf(p.manifest) == p.manifestSha256 && hashOf(p.bytes) == p.sha256 && p.bytes.size() == p.size);
        gPkgs[name] = p;
    }
    // Packages read with a detached signature, under "detached/<label>": the offer carries that signature, not the
    // archive's manifest.sig. Only the ones a reader takes are loaded.
    json::Value detached = doc.root().find("detached");
    for (size_t i = 0; i < detached.size(); ++i) {
        json::Value d = detached.at(i);
        if (!d.find("code").string().empty()) continue;
        std::string file = d.find("file").string();
        std::string name = file.substr(0, file.size() - 7);   // "detached/<label>"
        Pkg p;
        p.bytes = testutil::readFile(gData + "/" + file);
        p.signature = d.find("signature").string();
        std::string signature(kEd25519SignatureBytes, '\0');
        CHECK_IN(name, fromHex(p.signature, (uint8_t*)&signature[0], signature.size()));
        MemorySource source(p.bytes.data(), p.bytes.size());
        Package package;
        Error error;
        CHECK_IN(name + ": " + error.message, package.open(&source, both, signature, &error));
        p.id = package.manifest().id;
        p.version = package.manifest().version;
        p.sha256 = package.sha256();
        p.size = package.size();
        p.manifestSha256 = package.manifestSha256();
        p.manifest = package.manifestBytes();
        gPkgs[name] = p;
    }
}

// ---- JSON, read back ----------------------------------------------------------------------------------------------

// A message as the test looks at it. Not to be copied: the values point into it.
struct Parsed {
    json::Document doc;
    json::Value root, result, error, params;

    explicit Parsed(const std::string& text) {
        std::string why;
        CHECK_IN(why + ": " + text, doc.parse(text, &why));
        root = doc.root();
        result = root.find("result");
        error = root.find("error");
        params = root.find("params");
    }
    // "" for a result, else the error's code.
    std::string code() const { return error.isObject() ? error.find("code").string() : std::string(); }

private:
    Parsed(const Parsed&);
    Parsed& operator=(const Parsed&);
};

static std::string str(json::Value object, const char* name) {
    json::Value v = object.find(name);
    return v.isString() ? v.string() : std::string("<none>");
}

static long long integer(json::Value object, const char* name) {
    json::Value v = object.find(name);
    return v.isInteger() ? (long long)v.integer() : -1;
}

// ---- a phone ------------------------------------------------------------------------------------------------------

class Phone {
public:
    explicit Phone(int fd)
        : closed(false), maxFrame(65536), dataFrame(65536), serve(true), stallAt(-1), errorAt(-1), lieAboutSize(0),
          streamed(0), record(false), answerPings(true), pings(0), deaf(false), frames(0), fd_(fd), nextId_(1), nextStream_(1),
          streaming_(false), stream_(0), sent_(0) {
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        out_ = "NBTL";
    }
    ~Phone() { hangUp(); }

    void hangUp() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        closed = true;
    }

    void sendFrame(unsigned type, unsigned flags, unsigned stream, const std::string& payload) {
        std::string frame;
        frame += (char)type;
        frame += (char)flags;
        frame += (char)(stream & 0xFF);
        frame += (char)(stream >> 8);
        for (int i = 0; i < 4; ++i) frame += (char)((payload.size() >> (8 * i)) & 0xFF);
        frame += payload;
        unsigned long crc = ::crc32(0L, (const Bytef*)frame.data(), (uInt)frame.size());
        for (int i = 0; i < 4; ++i) frame += (char)((crc >> (8 * i)) & 0xFF);
        out_ += frame;
        note('>', type, payload, frame);
    }
    void sendRaw(const std::string& bytes) { out_ += bytes; }
    void sendMessage(const std::string& text) {
        lastSent = text;
        sendFrame(1, 0, 0, text);
    }

    int64_t request(const std::string& method, const std::string& params) {
        int64_t id = nextId_++;
        sendMessage("{\"id\":" + num(id) + ",\"method\":\"" + method + "\",\"params\":" + params + "}");
        return id;
    }
    void event(const std::string& name, const std::string& params) {
        sendMessage("{\"event\":\"" + name + "\",\"params\":" + params + "}");
    }
    // For a request the test writes out by hand: the id it has to carry, and then that the id is used up.
    int64_t peekId() const { return nextId_; }
    void skipId() { ++nextId_; }

    // Writes what is queued, reads what came, and feeds an open stream. True if a byte moved.
    bool pump() {
        if (fd_ < 0) return false;
        bool moved = false;
        feed();
        while (!out_.empty()) {
            ssize_t n = write(fd_, out_.data(), out_.size());
            if (n <= 0) break;
            out_.erase(0, (size_t)n);
            moved = true;
            feed();
        }
        char buffer[65536];
        while (!deaf) {
            ssize_t n = read(fd_, buffer, sizeof buffer);
            if (n > 0) {
                in_.append(buffer, (size_t)n);
                moved = true;
                continue;
            }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) closed = true;
            break;
        }
        while (in_.size() >= 12) {
            const unsigned char* p = (const unsigned char*)in_.data();
            size_t length = p[4] | (p[5] << 8) | (p[6] << 16) | ((size_t)p[7] << 24);
            CHECK(length <= maxFrame);   // the agent keeps to the frame size hello set
            if (in_.size() < 12 + length) break;
            unsigned long crc = ::crc32(0L, p, (uInt)(8 + length));
            unsigned long theirs = p[8 + length] | (p[9 + length] << 8) | (p[10 + length] << 16) | ((unsigned long)p[11 + length] << 24);
            CHECK(crc == theirs);
            std::string payload = in_.substr(8, length);
            note('<', p[0], payload, in_.substr(0, 12 + length));
            if (p[0] == 2) {
                // DATA of a files stream: bit 1 of the flags marks the last frame, and bit 0 stays reserved.
                CHECK((p[1] & ~2) == 0);
                unsigned stream = p[2] | (p[3] << 8);
                bool last = (p[1] & 2) != 0;
                CHECK_IN(num(stream), stream != 0 && ended.count(stream) == 0);
                received[stream] += payload;
                ++frames;
                if (last) ended.insert(stream);
                heard.push_back("D" + num(stream) + (last ? " last" : ""));
                in_.erase(0, 12 + length);
                continue;
            }
            CHECK(p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 0);   // a message
            in_.erase(0, 12 + length);
            handle(payload);
        }
        return moved;
    }

    // The reply to a request, taken out of the box. The test fails if it has not come.
    std::string reply(int64_t id) {
        CHECK_IN("no reply to request " + num(id), replies.count(id) != 0);
        std::string text = replies[id];
        replies.erase(id);
        return text;
    }
    bool streaming() const { return streaming_; }
    // Answers a blob.fetch that was held back while `serve` was false.
    void answerHeld() {
        std::vector<std::string> held = heldFetches;
        heldFetches.clear();
        for (size_t i = 0; i < held.size(); ++i) {
            Parsed m(held[i]);
            fetch(m.root.find("id").integer(), m.params);
        }
    }

    bool closed;
    uint32_t maxFrame;                           // what this phone said in hello
    uint32_t dataFrame;                          // the most it puts in one DATA frame
    std::map<int64_t, std::string> replies;      // by request id: the whole message
    std::vector<std::string> events;             // whole messages, in order
    std::map<std::string, std::string> blobs;    // what it serves, by hash
    std::vector<std::string> fetches;            // every blob.fetch it was asked: "sha256 offset length|-"
    std::vector<std::string> heldFetches;
    bool serve;                                  // false: blob.fetch requests are kept and not answered
    std::string failNext;                        // answer the next blob.fetch with this error code
    long long stallAt;                           // stop sending a stream after this many of its bytes
    long long errorAt;                           // send blob.error after this many bytes of a stream
    std::string errorCode;
    long long lieAboutSize;                      // added to the `size` of the next blob.fetch reply
    uint64_t streamed;                           // DATA bytes sent in all
    bool record;
    std::string transcript;
    bool answerPings;                            // false: the agent's pings are counted and get no answer
    unsigned pings;                              // how many the agent has sent
    bool deaf;                                   // true: it writes and does not read, so the agent's side fills up
    std::string lastHeard;                       // the payload of the last message that came, of whatever form
    std::string lastSent;                        // the payload of the last message it sent
    std::string helloSent, helloReply;           // the payloads of its hello and of the answer: what `auth` covers
    std::map<unsigned, std::string> received;    // the DATA the agent sent, by stream
    std::set<unsigned> ended;                    // the streams whose last frame has come
    unsigned frames;                             // DATA frames from the agent in all
    std::vector<std::string> heard;              // what came, in order: "D<stream>[ last]", "R<id>", "E<event>"

private:
    void note(char direction, unsigned type, const std::string& payload, const std::string& frame) {
        if (!record) return;
        if (type == 1) transcript += "# " + payload + "\n";
        else transcript += "# DATA, " + num((long long)payload.size()) + " bytes\n";
        transcript += std::string(1, direction) + " " + toHex((const uint8_t*)frame.data(), frame.size()) + "\n";
    }

    void handle(const std::string& text) {
        Parsed m(text);
        lastHeard = text;
        if (m.root.find("method").isString()) {
            if (m.root.find("method").string() == "ping") {
                CHECK_IN(text, m.params.isObject() && m.params.size() == 0);
                ++pings;
                if (answerPings) sendMessage("{\"id\":" + num(m.root.find("id").integer()) + ",\"result\":{}}");
                return;
            }
            CHECK_IN(text, m.root.find("method").string() == "blob.fetch");
            if (!serve) {
                heldFetches.push_back(text);
                return;
            }
            fetch(m.root.find("id").integer(), m.params);
        } else if (m.root.find("event").isString()) {
            events.push_back(text);
            heard.push_back("E" + m.root.find("event").string());
        } else {
            CHECK_IN(text, m.root.find("id").isInteger());
            replies[m.root.find("id").integer()] = text;
            heard.push_back("R" + num(m.root.find("id").integer()));
        }
    }

    void fetch(int64_t id, json::Value params) {
        std::string sha = params.find("sha256").string();
        uint64_t offset = (uint64_t)params.find("offset").integer();
        json::Value length = params.find("length");
        fetches.push_back(sha + " " + num((long long)offset) + " " + (length.isInteger() ? num(length.integer()) : std::string("-")));
        std::string code = failNext;
        failNext.clear();
        std::map<std::string, std::string>::const_iterator it = blobs.find(sha);
        if (code.empty() && it == blobs.end()) code = "not-found";
        if (code.empty() && streaming_) code = "busy";
        if (code.empty()) {
            uint64_t size = it->second.size();
            uint64_t count = length.isInteger() ? (uint64_t)length.integer() : (offset < size ? size - offset : 0);
            if (offset >= size || count > size - offset) code = "out-of-range";
            else pending_ = it->second.substr((size_t)offset, (size_t)count);
        }
        if (!code.empty()) {
            sendMessage("{\"id\":" + num(id) + ",\"error\":{\"code\":\"" + code + "\",\"message\":\"test phone\"}}");
            return;
        }
        stream_ = nextStream_++;
        sent_ = 0;
        streaming_ = true;
        sendMessage("{\"id\":" + num(id) + ",\"result\":{\"stream\":" + num(stream_) + ",\"size\":" +
                    num((long long)pending_.size() + lieAboutSize) + "}}");
        lieAboutSize = 0;
    }

    // One DATA frame at a time, and only when the last one has gone out.
    void feed() {
        while (streaming_ && out_.empty()) {
            if (errorAt >= 0 && (long long)sent_ >= errorAt) {
                event("blob.error", "{\"stream\":" + num(stream_) + ",\"code\":\"" + errorCode + "\",\"message\":\"test phone\"}");
                errorAt = -1;
                streaming_ = false;
                return;
            }
            if (stallAt >= 0 && (long long)sent_ >= stallAt) return;
            size_t n = pending_.size() - sent_;
            if (n > maxFrame) n = maxFrame;
            if (n > dataFrame) n = dataFrame;
            if (errorAt >= 0 && (long long)(sent_ + n) > errorAt) n = (size_t)(errorAt - (long long)sent_);
            if (stallAt >= 0 && (long long)(sent_ + n) > stallAt) n = (size_t)(stallAt - (long long)sent_);
            bool last = sent_ + n == pending_.size();
            sendFrame(2, last ? 2 : 0, stream_, pending_.substr(sent_, n));
            sent_ += n;
            streamed += n;
            if (last) streaming_ = false;
        }
    }

    int fd_;
    std::string out_, in_;
    int64_t nextId_;
    unsigned nextStream_;
    bool streaming_;
    unsigned stream_;
    std::string pending_;
    size_t sent_;
};

// ---- a head unit in a folder, with its agent ----------------------------------------------------------------------

class FakeClock : public Clock {
public:
    FakeClock() : now(1000) {}
    virtual int64_t nowMs() { return now; }
    virtual int64_t unixSeconds() { return kPairedAt; }
    int64_t now;
    static const int64_t kPairedAt = 1794400000;   // the date every pairing of a test gets
};

// 00 01 02 ...: the head-unit id, the plan ids, and the codes, keys and nonces come out the same in every run.
class FakeRandom : public RandomSource {
public:
    FakeRandom() : next(0), noSecret(false) {}
    virtual void bytes(uint8_t* out, size_t len) {
        for (size_t i = 0; i < len; ++i) out[i] = next++;
    }
    virtual bool secret(uint8_t* out, size_t len) {
        if (noSecret) return false;
        bytes(out, len);
        return true;
    }
    uint8_t next;
    bool noSecret;   // a machine whose /dev/urandom cannot be read
};

static const char kHeadUnitId[] = "000102030405060708090a0b0c0d0e0f";

struct Bench {
    std::string root;
    HostPlatform platform;
    FakeClock clock;
    FakeRandom random;
    Agent agent;
    std::vector<Phone*> phones;

    static std::vector<TrustedKey> keys(bool both) {
        std::vector<TrustedKey> k;
        k.push_back(gPackageKey);
        if (both) k.push_back(gOtherKey);
        return k;
    }

    // A new, empty head unit; or, with `reuse`, the one that is in the folder already, as after a restart.
    explicit Bench(const std::string& name, bool reuse = false, bool bothKeys = false, const char* version = "1.0.0")
        : root(gWork + "/" + name), platform(root), agent(&platform, keys(bothKeys), version, &clock, &random) {
        std::string why;
        if (!reuse) CHECK_IN(why, fs::removeTree(root, &why));
        CHECK_IN(why, platform.prepare(&why));
        agent.setLog(0);
        CHECK_IN(why, agent.init(&why));
    }
    ~Bench() {
        for (size_t i = 0; i < phones.size(); ++i) delete phones[i];
    }

    // A link with nothing said on it yet.
    Phone* connect(const char* kind = "local") {
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        agent.adopt(new SocketLink(fds[0], kind));
        Phone* phone = new Phone(fds[1]);
        phones.push_back(phone);
        return phone;
    }

    // A link on which a phone has said hello, and nothing more.
    Phone* greeted(const char* idDigit = "1", unsigned maxFrame = 65536, const char* kind = "local") {
        Phone* p = connect(kind);
        p->maxFrame = maxFrame;
        std::string text = hello(p, phoneHello(idDigit, maxFrame));
        CHECK_IN(text, Parsed(text).result.isObject());
        return p;
    }

    // A phone that may use every method and serves every test package. On a link that is not the local socket
    // it is paired first, with the key that keyOf() gives its id, and has done `auth`.
    Phone* phone(const char* idDigit = "1", unsigned maxFrame = 65536, const char* kind = "local") {
        Phone* p = greeted(idDigit, maxFrame, kind);
        if (strcmp(kind, "local") != 0) {
            pair(idDigit);
            std::string text = authenticate(p, keyOf(idDigit));
            CHECK_IN(text, Parsed(text).result.isObject());
        }
        for (std::map<std::string, Pkg>::const_iterator it = gPkgs.begin(); it != gPkgs.end(); ++it) {
            p->blobs[it->second.sha256] = it->second.bytes;
            p->blobs[it->second.manifestSha256] = it->second.manifest;
        }
        return p;
    }

    static std::string phoneHello(const char* idDigit, unsigned maxFrame) {
        return "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":\"" + std::string(32, idDigit[0]) +
               "\",\"name\":\"Test phone\",\"maxFrame\":" + num(maxFrame) + ",\"capabilities\":[]}";
    }

    // hello, with the two payloads kept that `auth` covers. Returns the answer.
    std::string hello(Phone* p, const std::string& params) {
        int64_t id = p->request("hello", params);
        p->helloSent = p->lastSent;
        settle();
        p->helloReply = p->reply(id);
        return p->helloReply;
    }

    // The pairing key a test gives the phone with this id: the id's digit, 32 times.
    static std::string keyOf(const char* idDigit) { return std::string(kPairingKeyBytes, idDigit[0]); }

    // Pairs a phone without the window: the record is written as pairing.issue writes it.
    void pair(const char* idDigit, const std::string& key = std::string()) {
        PairedPhone paired;
        paired.id = std::string(32, idDigit[0]);
        paired.name = "Test phone";
        paired.pairedAt = FakeClock::kPairedAt;
        memcpy(paired.key, (key.empty() ? keyOf(idDigit) : key).data(), kPairingKeyBytes);
        std::string why;
        CHECK_IN(why, agent.phones().put(paired, &why));
    }

    // A proof, as the phone works it out: HMAC-SHA256(key, label || A || B || SHA-256(hello) || SHA-256(reply)).
    static std::string proof(const Phone* p, const std::string& key, const char* label, const std::string& nonceA,
                             const std::string& nonceB) {
        uint8_t a[kAuthNonceBytes], b[kAuthNonceBytes], sent[kSha256Bytes], reply[kSha256Bytes], out[kSha256Bytes];
        CHECK(fromHex(nonceA, a, sizeof a) && fromHex(nonceB, b, sizeof b));
        sha256(p->helloSent.data(), p->helloSent.size(), sent);
        sha256(p->helloReply.data(), p->helloReply.size(), reply);
        HmacSha256 mac(key.data(), key.size());
        mac.update(label, strlen(label));
        mac.update(a, sizeof a);
        mac.update(b, sizeof b);
        mac.update(sent, sizeof sent);
        mac.update(reply, sizeof reply);
        mac.finish(out);
        return toHex(out, sizeof out);
    }

    // Both calls of `auth` with this key. Returns the answer to the second, or to the first if that was an error.
    // The agent's own proof has to be the right one.
    std::string authenticate(Phone* p, const std::string& key, const std::string& nonce = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf") {
        std::string first = call(p, "auth", "{\"nonce\":\"" + nonce + "\"}");
        Parsed r(first);
        if (!r.result.isObject()) return first;
        std::string theirs = str(r.result, "nonce");
        CHECK_IN(first, theirs.size() == 2 * kAuthNonceBytes && str(r.result, "proof") == proof(p, key, "nbt-auth-1", nonce, theirs));
        return call(p, "auth", "{\"proof\":\"" + proof(p, key, "nbt-auth-2", nonce, theirs) + "\"}");
    }

    // Until nothing moves any more: no byte on any link, and no install step left to run.
    void settle() {
        int quiet = 0;
        for (int round = 0; round < 2000000 && quiet < 3; ++round) {
            bool moved = false;
            for (size_t i = 0; i < phones.size(); ++i) moved = phones[i]->pump() || moved;
            agent.turn(0);
            for (size_t i = 0; i < phones.size(); ++i) moved = phones[i]->pump() || moved;
            moved = moved || agent.busy();
            quiet = moved ? 0 : quiet + 1;
        }
        CHECK(quiet >= 3);
    }

    std::string call(Phone* phone, const std::string& method, const std::string& params) {
        int64_t id = phone->request(method, params);
        settle();
        return phone->reply(id);
    }

    std::string app(const std::string& id) const { return platform.appDir(id); }
    bool installed(const std::string& id) { return agent.engine().store().isInstalled(id); }
    std::string versionOf(const std::string& id) {
        InstalledPackage p;
        std::string why;
        CHECK_IN(why, agent.engine().store().load(id, &p, &why));
        return p.manifest.version;
    }
};

static std::string offer(const Pkg& p) {
    return "{\"id\":\"" + p.id + "\",\"version\":\"" + p.version + "\",\"sha256\":\"" + p.sha256 + "\",\"size\":" +
           num((long long)p.size) + ",\"manifestSha256\":\"" + p.manifestSha256 + "\",\"signature\":\"" + p.signature + "\"}";
}

// The parameters of job.plan. `install` and `offers` name test packages ("org.nbtapp.hello-1.2.0"); `remove` names
// ids, with a ! at the end for deleteData.
static std::string planParams(const std::string& install, const std::string& remove, const std::string& offers) {
    std::string out = "{\"install\":[";
    std::vector<std::string> list = words(install);
    for (size_t i = 0; i < list.size(); ++i) {
        out += std::string(i == 0 ? "" : ",") + "{\"id\":\"" + pkg(list[i]).id + "\",\"version\":\"" + pkg(list[i]).version + "\"}";
    }
    out += "],\"remove\":[";
    list = words(remove);
    for (size_t i = 0; i < list.size(); ++i) {
        bool deleteData = list[i][list[i].size() - 1] == '!';
        std::string id = deleteData ? list[i].substr(0, list[i].size() - 1) : list[i];
        out += std::string(i == 0 ? "" : ",") + "{\"id\":\"" + id + "\",\"deleteData\":" + (deleteData ? "true" : "false") + "}";
    }
    out += "],\"offers\":[";
    list = words(offers);
    for (size_t i = 0; i < list.size(); ++i) out += (i == 0 ? "" : ",") + offer(pkg(list[i]));
    return out + "]}";
}

static const char kHello[] = "org.nbtapp.hello-1.2.0";
static const char kHelloOld[] = "org.nbtapp.hello-1.1.0";
static const char kRuntime[] = "org.nbtapp.runtime-2.1.0";
static const char kHelloAndRuntime[] = "org.nbtapp.hello-1.2.0 org.nbtapp.runtime-2.1.0";

// Plans and expects a plan that can run; returns its id.
static std::string planOk(Bench& b, Phone* p, const std::string& install, const std::string& remove, const std::string& offers) {
    std::string text = b.call(p, "job.plan", planParams(install, remove, offers));
    Parsed r(text);
    CHECK_IN(text, r.result.isObject() && r.result.find("planId").isString());
    CHECK_IN(text, r.result.find("problems").size() == 0 && r.result.find("missing").size() == 0);
    return r.result.find("planId").string();
}

// Plans and expects exactly one problem, with this code.
static void planRefused(Bench& b, Phone* p, const std::string& params, const char* code, const char* id = 0) {
    std::string text = b.call(p, "job.plan", params);
    Parsed r(text);
    CHECK_IN(text, r.result.isObject() && !r.result.find("planId").present());
    json::Value problems = r.result.find("problems");
    CHECK_IN(text, problems.size() == 1 && str(problems.at(0), "code") == code);
    if (id != 0) CHECK_IN(text, str(problems.at(0), "id") == id);
}

static std::string startJob(Bench& b, Phone* p, const std::string& planId) { return b.call(p, "job.start", "{\"planId\":\"" + planId + "\"}"); }

// The last event of a name that the phone has seen, or "".
static std::string lastEvent(const Phone* p, const char* name) {
    for (size_t i = p->events.size(); i-- > 0;) {
        if (has(p->events[i], std::string("\"event\":\"") + name + "\"")) return p->events[i];
    }
    return std::string();
}

// The reason of the link.closing a phone was sent, or "" if it was sent none. The event is the last thing the agent
// says on a link, has the reason and nothing else, and is not numbered.
static std::string closingReason(const Phone* p) {
    std::string text = lastEvent(p, "link.closing");
    if (text.empty()) return text;
    Parsed e(text);
    CHECK_IN(text + " / " + p->lastHeard, p->lastHeard == text && e.params.size() == 1 && !e.root.find("seq").present());
    return str(e.params, "reason");
}

// How the job ended, from job.done: its state, then each step's result (and error code), separated by spaces.
static std::string outcome(const Phone* p) {
    std::string text = lastEvent(p, "job.done");
    if (text.empty()) return "no job.done";
    Parsed e(text);
    std::string out = str(e.params, "state");
    json::Value steps = e.params.find("steps");
    for (size_t i = 0; i < steps.size(); ++i) {
        out += " " + str(steps.at(i), "result");
        if (steps.at(i).find("error").isObject()) out += "(" + str(steps.at(i).find("error"), "code") + ")";
    }
    return out;
}

// Installs packages through a whole job and expects it to succeed.
static void install(Bench& b, Phone* p, const std::string& targets, const std::string& offers) {
    p->events.clear();
    std::string planId = planOk(b, p, targets, "", offers);
    Parsed r(startJob(b, p, planId));
    CHECK_IN(r.code(), r.result.isObject());
    CHECK_IN(outcome(p), has(outcome(p), "succeeded") && !has(outcome(p), "failed") && !has(outcome(p), "skipped"));
}

// ---- hello, and what ends a link ----------------------------------------------------------------------------------

static const char kGoodHello[] = "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":65536,\"capabilities\":[]}";

static void testHello() {
    Bench b("hello");
    Phone* p = b.connect();
    // Anything before hello is refused, and the link stays.
    CHECK(Parsed(b.call(p, "info.get", "{}")).code() == "bad-state");
    const char* bad[] = {
        "{}",
        "{\"protocolMin\":0,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":65536,\"capabilities\":[]}",
        "{\"protocolMin\":2,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":65536,\"capabilities\":[]}",
        "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"agent\",\"maxFrame\":65536,\"capabilities\":[]}",
        "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":1023,\"capabilities\":[]}",
        "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":65537,\"capabilities\":[]}",
        "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":65536}",
        "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":65536,\"capabilities\":[1]}",
        "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"maxFrame\":65536,\"capabilities\":[]}",
        "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":\"ABCDEF0123456789abcdef0123456789\",\"name\":\"x\",\"maxFrame\":65536,\"capabilities\":[]}",
        "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":\"11111111111111111111111111111111\",\"name\":\"\",\"maxFrame\":65536,\"capabilities\":[]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK_IN(bad[i], Parsed(b.call(p, "hello", bad[i])).code() == "bad-request");
    CHECK(!p->closed);

    // A newer protocol range on top of version 1 is fine; unknown fields and capabilities are ignored.
    Parsed r(b.call(p, "hello", "{\"protocolMin\":1,\"protocolMax\":7,\"role\":\"cli\",\"maxFrame\":4096,\"capabilities\":[\"zstd\"],\"extra\":{\"a\":[1]}}"));
    CHECK(integer(r.result, "protocolMin") == 1 && integer(r.result, "protocolMax") == 1 && str(r.result, "role") == "agent");
    CHECK(str(r.result, "headUnitId") == kHeadUnitId && str(r.result, "agentVersion") == "1.0.0");
    CHECK(integer(r.result, "maxFrame") == 65536 && r.result.find("capabilities").isArray() && r.result.find("capabilities").size() == 3 &&
          r.result.find("capabilities").at(0).string() == kCapabilityAutostart &&
          r.result.find("capabilities").at(1).string() == kCapabilityDetachedSignature &&
          r.result.find("capabilities").at(2).string() == kCapabilityFiles);
    CHECK(Parsed(b.call(p, "hello", kGoodHello)).code() == "bad-state");
    // Methods that are not there.
    const char* missing[] = {"job.frobnicate", "pairing.frobnicate", ""};
    for (size_t i = 0; i < sizeof missing / sizeof missing[0]; ++i) CHECK(Parsed(b.call(p, missing[i], "{}")).code() == "unsupported");
    // The two that belong to a phone's own link: the local socket has neither.
    CHECK(Parsed(b.call(p, "auth", "{\"nonce\":\"000102030405060708090a0b0c0d0e0f\"}")).code() == "forbidden");
    CHECK(Parsed(b.call(p, "pairing.issue", "{\"code\":\"123456\"}")).code() == "forbidden" && !p->closed);
    // ping: an empty result, whatever it is given; and like everything else, not before hello.
    Parsed pong(b.call(p, "ping", "{}"));
    CHECK(pong.result.isObject() && pong.result.size() == 0);
    CHECK(Parsed(b.call(p, "ping", "{\"anything\":1}")).result.isObject());
    Phone* early = b.connect();
    CHECK(Parsed(b.call(early, "ping", "{}")).code() == "bad-state" && !early->closed);
    // A request with no params at all, params that are not an object, and an event nobody knows.
    p->sendMessage("{\"id\":900,\"method\":\"sources.list\"}");
    p->sendMessage("{\"id\":901,\"method\":\"sources.list\",\"params\":[]}");
    p->event("weather.changed", "{\"rain\":true}");
    p->sendMessage("{\"id\":77,\"result\":{}}");   // an answer to nothing the agent asked
    b.settle();
    CHECK(Parsed(p->reply(900)).result.isObject() && Parsed(p->reply(901)).code() == "bad-request" && !p->closed);

    // No version in common: the answer says so, and the link closes.
    Phone* future = b.connect();
    CHECK(Parsed(b.call(future, "hello", "{\"protocolMin\":2,\"protocolMax\":3,\"role\":\"cli\",\"maxFrame\":65536,\"capabilities\":[]}")).code() == "unsupported-version");
    CHECK(future->closed);

    // The head-unit id is made once and kept.
    Bench again("hello", true);
    CHECK(again.agent.headUnitId() == kHeadUnitId);
}

// Each of these ends the link without an answer.
static void testLinkEnds() {
    Bench b("link-ends");
    const char* cases[] = {"type", "frame", "json", "form", "request-id", "data", "big"};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        std::string what = cases[i];
        Phone* p = b.connect();
        if (what == "type") {
            p->sendRaw("XXXXXXXX");   // after the magic bytes, a frame header of a type that does not exist
        } else {
            CHECK(Parsed(b.call(p, "hello", "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":2048,\"capabilities\":[]}")).result.isObject());
            if (what == "frame") p->sendRaw(std::string("\x01\x00\x00\x00\x02\x00\x00\x00{}\x00\x00\x00\x00", 14));
            if (what == "json") p->sendMessage("{\"id\":1,\"method\":\"info.get\",\"params\":{\"a\":1,\"a\":2}}");
            if (what == "form") p->sendMessage("{\"hello\":\"there\"}");
            if (what == "request-id") p->sendMessage("{\"id\":0,\"method\":\"info.get\",\"params\":{}}");
            if (what == "data") p->sendFrame(2, 2, 9, "bytes nobody asked for");
            if (what == "big") p->sendMessage("{\"id\":5,\"method\":\"info.get\",\"params\":{\"pad\":\"" + std::string(2048, 'x') + "\"}}");
        }
        b.settle();
        CHECK_IN(what, p->closed && p->replies.empty());
    }
    // A link that does not start with the magic bytes at all.
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    b.agent.adopt(new SocketLink(fds[0], "local"));
    CHECK(write(fds[1], "GET / HTTP/1.0\r\n\r\n", 18) == 18);
    b.settle();
    char c;
    CHECK(read(fds[1], &c, 1) == 0);   // closed, and nothing was sent
    close(fds[1]);
    b.settle();
    CHECK(b.agent.sessions() == 0);

    // A newer link from the same phone replaces the older one; another phone is left alone.
    Phone* first = b.phone("1");
    Phone* other = b.phone("2");
    Phone* second = b.phone("1");
    b.settle();
    CHECK(first->closed && !other->closed && !second->closed && b.agent.sessions() == 2);
}

// ---- a Bluetooth serial link: the smaller frame, and the keep-alive ----------------------------------------------

static std::string helloOf(unsigned maxFrame) {
    return "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"cli\",\"maxFrame\":" + num(maxFrame) + ",\"capabilities\":[]}";
}

// A request whose whole message is `bytes` long.
static std::string paddedInfoGet(Phone* p, size_t bytes) {
    std::string open = "{\"id\":" + num(p->peekId()) + ",\"method\":\"info.get\",\"params\":{\"pad\":\"", close = "\"}}";
    CHECK(bytes > open.size() + close.size());
    return open + std::string(bytes - open.size() - close.size(), 'x') + close;
}

// Passes time inside an install step, at the points where a crash test would end the process.
class SlowStep : public CrashHook {
public:
    SlowStep() : bench(0), msEach(0), points(0) {}
    virtual void point(const char*) {
        ++points;
        bench->clock.now += msEach;
        for (size_t i = 0; i < bench->phones.size(); ++i) bench->phones[i]->pump();
    }
    Bench* bench;
    int64_t msEach;
    int points;
};

static void testSerialLink() {
    {   // hello: on a serial link the agent gives 4,096, and the lower of the two values holds both ways
        Bench b("serial-frame");
        Phone* p = b.phone("1", 65536, "serial");
        p->maxFrame = 4096;   // what the phone checks the agent's frames against from here on
        CHECK(integer(Parsed(p->helloReply).result, "maxFrame") == (long long)kSerialMaxFrame && kSerialMaxFrame == 4096);
        // A message of exactly that size is taken, and one byte more ends the link.
        int64_t id = p->peekId();
        p->sendMessage(paddedInfoGet(p, 4096));
        p->skipId();
        b.settle();
        CHECK(Parsed(p->reply(id)).result.isObject() && !p->closed);
        p->sendMessage(paddedInfoGet(p, 4097));
        b.settle();
        CHECK(p->closed);

        // A phone that asks for less gets the same answer, and its own lower value holds.
        Phone* small = b.phone("2", 2048, "serial");
        CHECK(integer(Parsed(small->helloReply).result, "maxFrame") == 4096);
        id = small->peekId();
        small->sendMessage(paddedInfoGet(small, 2048));
        small->skipId();
        b.settle();
        CHECK(Parsed(small->reply(id)).result.isObject() && !small->closed);
        small->sendMessage(paddedInfoGet(small, 2049));
        b.settle();
        CHECK(small->closed);

        // Any other kind of link: 65,536, as before.
        Phone* local = b.connect();
        CHECK(integer(Parsed(b.call(local, "hello", helloOf(4096))).result, "maxFrame") == 65536);
    }
    {   // A whole install through 4,096-byte frames: every frame the agent sends keeps to it (Phone::pump checks),
        // and so does every DATA frame it is sent.
        Bench b("serial-install");
        Phone* p = b.phone("1", 65536, "serial");
        p->maxFrame = 4096;
        install(b, p, kHelloAndRuntime, kHelloAndRuntime);
        CHECK(b.installed("org.nbtapp.hello") && b.installed("org.nbtapp.runtime") && !p->closed);
    }
    {   // The keep-alive: a ping after 10 s with nothing heard, and as long as the phone answers the link stays.
        Bench b("serial-ping");
        Phone* p = b.phone("1", 4096, "serial");
        b.clock.now += kSerialPingAfterMs - 1;
        b.settle();
        CHECK(p->pings == 0);
        b.clock.now += 1;
        b.settle();
        CHECK(p->pings == 1 && !p->closed);
        for (int i = 0; i < 20; ++i) {   // more than three minutes of nothing but pings
            b.clock.now += kSerialPingAfterMs;
            b.settle();
        }
        CHECK(p->pings == 21 && !p->closed && b.agent.sessions() == 1);
        // Anything the phone sends counts as heard: no ping while it talks, however little.
        for (int i = 0; i < 10; ++i) {
            b.clock.now += kSerialPingAfterMs - 1;
            CHECK(Parsed(b.call(p, "ping", "{}")).result.isObject());
        }
        CHECK(p->pings == 21);

        // A phone that has gone: two pings, and at 30 s the link is ended.
        p->answerPings = false;
        b.clock.now += kSerialPingAfterMs;
        b.settle();
        CHECK(p->pings == 22 && !p->closed);
        b.clock.now += kSerialPingAfterMs;
        b.settle();
        CHECK(p->pings == 23 && !p->closed);
        b.clock.now += kSerialSilenceMs - 2 * kSerialPingAfterMs - 1;
        b.settle();
        CHECK(p->pings == 23 && !p->closed);
        b.clock.now += 1;
        b.settle();
        CHECK(p->closed && b.agent.sessions() == 0);
        // It is told so, in case it is there after all and holds the link: the phone is the one to end it.
        CHECK(closingReason(p) == "silent");
    }
    {   // No hello within 30 s of opening: ended, whether the link says nothing at all, says something that is
        // not hello, or stops halfway into the magic bytes.
        Bench b("serial-no-hello");
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        b.agent.adopt(new SocketLink(fds[0], "serial"));
        Phone* talker = b.connect("serial");
        int half[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, half) == 0);
        b.agent.adopt(new SocketLink(half[0], "serial"));
        CHECK(write(half[1], "NB", 2) == 2);
        for (int i = 0; i < 3; ++i) {
            b.clock.now += i < 2 ? 10000 : 9999;
            CHECK(Parsed(b.call(talker, "info.get", "{}")).code() == "bad-state");
        }
        CHECK(b.agent.sessions() == 3 && talker->pings == 0);
        b.clock.now += 1;
        b.settle();
        CHECK(b.agent.sessions() == 0 && talker->closed && talker->events.empty());
        char c;
        CHECK(read(fds[1], &c, 1) == 0 && read(half[1], &c, 1) == 0);   // closed, and not a byte was sent
        close(fds[1]);
        close(half[1]);
    }
    {   // Links of another kind are left alone: no ping, and no end, with or without hello.
        Bench b("serial-not");
        Phone* greeted = b.phone();
        Phone* silent = b.connect();
        b.clock.now += 3600 * 1000;
        b.settle();
        CHECK(!greeted->closed && !silent->closed && greeted->pings == 0 && b.agent.sessions() == 2);
    }
    {   // A job's phone goes silent in the middle of a fetch: the link is ended at 30 s, not at the fetch's own
        // timeout, and the job waits as paused.
        Bench b("serial-fetch");
        Phone* p = b.phone("1", 4096, "serial");
        std::string planId = planOk(b, p, kRuntime, "", kRuntime);
        p->serve = false;
        p->answerPings = false;
        startJob(b, p, planId);
        for (int i = 0; i < 2; ++i) {
            b.clock.now += kSerialPingAfterMs;
            b.settle();
            CHECK(!p->closed && b.agent.job().fetching());
        }
        b.clock.now += kSerialPingAfterMs;
        b.settle();
        CHECK(p->closed && b.agent.job().paused() && closingReason(p) == "silent");
        // A phone that answers pings and not the fetch keeps its link until the fetch's timeout.
        Phone* q = b.phone("1", 4096, "serial");
        q->serve = false;
        startJob(b, q, planId);
        for (int i = 0; i < 5; ++i) {
            b.clock.now += kSerialPingAfterMs;
            b.settle();
            CHECK(!q->closed && b.agent.job().fetching());
        }
        b.clock.now += kSerialPingAfterMs + 1;
        b.settle();
        CHECK(q->closed && b.agent.job().paused() && q->pings >= 5 && closingReason(q) == "silent");
    }
    {   // Inside an install step the agent goes on serving its links, and the keep-alive with them: the phone that
        // answers is pinged and stays, the one that does not is ended, and the step finishes either way.
        Bench b("serial-step");
        Phone* owner = b.phone("1", 4096, "serial");
        Phone* gone = b.phone("2", 4096, "serial");
        gone->answerPings = false;
        std::string planId = planOk(b, owner, kHello, "", kHelloAndRuntime);
        SlowStep slow;
        slow.bench = &b;
        slow.msEach = 4000;
        b.agent.engine().setCrashHook(&slow);
        owner->events.clear();
        startJob(b, owner, planId);
        b.agent.engine().setCrashHook(0);
        CHECK_IN(outcome(owner), slow.points >= 10 && has(outcome(owner), "succeeded"));
        CHECK(owner->pings >= 2 && !owner->closed && gone->closed && b.agent.sessions() == 1);
    }
}

// ---- auth: who a link that is not the local socket belongs to ----------------------------------------------------

static const char kNonce[] = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";

static std::string nonceParams(const std::string& nonce = kNonce) { return "{\"nonce\":\"" + nonce + "\"}"; }

static void testAuth() {
    Bench b("auth");
    {   // Before auth: hello, ping, and nothing else. What is refused says nothing about the head unit.
        Phone* p = b.greeted("1", 4096, "serial");
        const char* refused[] = {"info.get", "packages.list", "sources.list", "source.add", "job.plan", "job.start", "job.status",
                                 "job.cancel", "system.restart", "pairing.open", "pairing.close", "phones.list", "phones.forget",
                                 "job.frobnicate", ""};
        for (size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
            CHECK_IN(refused[i], Parsed(b.call(p, refused[i], "{}")).code() == "unauthenticated");
        }
        CHECK(Parsed(b.call(p, "ping", "{}")).result.isObject() && !p->closed);
        CHECK(Parsed(b.call(p, "hello", Bench::phoneHello("1", 4096))).code() == "bad-state");
        // It hears nothing of a job either, and is still kept alive.
        Phone* local = b.phone("9");
        install(b, local, kRuntime, kRuntime);
        CHECK(p->events.empty() && !local->events.empty());
        b.clock.now += kSerialPingAfterMs;
        b.settle();
        CHECK(p->pings == 1 && !p->closed);
        // What auth takes, and in which order. None of this ends the link.
        const char* bad[] = {"{}", "{\"nonce\":\"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf\",\"proof\":\"00\"}", "{\"nonce\":\"a0a1\"}",
                             "{\"nonce\":\"A0A1A2A3A4A5A6A7A8A9AAABACADAEAF\"}", "{\"nonce\":7}", "{\"proof\":\"00\"}", "{\"proof\":[]}"};
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK_IN(bad[i], Parsed(b.call(p, "auth", bad[i])).code() == "bad-request");
        CHECK(Parsed(b.call(p, "auth", "{\"proof\":\"" + std::string(64, '0') + "\"}")).code() == "bad-state" && !p->closed);
        // A phone the head unit has no key for: auth-failed, and the link is closed.
        CHECK(Parsed(b.call(p, "auth", nonceParams())).code() == "auth-failed");
        CHECK(p->closed && p->events.empty());   // the error is all it is told
        b.settle();
        CHECK(b.agent.sessions() == 1);
    }
    {   // A paired phone: two calls, and then every method. The agent's proof is checked by authenticate().
        b.pair("1");
        Phone* p = b.greeted("1", 4096, "serial");
        CHECK(Parsed(b.authenticate(p, Bench::keyOf("1"))).result.size() == 0);
        CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "headUnitId") == kHeadUnitId);
        CHECK(Parsed(b.call(p, "auth", nonceParams())).code() == "bad-state" && !p->closed);
        // The agent's nonce is new every time.
        Phone* q = b.greeted("1", 4096, "serial");
        Parsed first(b.call(q, "auth", nonceParams()));
        // A second nonce is not taken, and the proof for the first still is.
        CHECK(Parsed(b.call(q, "auth", nonceParams("00000000000000000000000000000000"))).code() == "bad-state");
        std::string theirs = str(first.result, "nonce");
        Phone* r = b.greeted("1", 4096, "serial");
        CHECK(str(Parsed(b.call(r, "auth", nonceParams())).result, "nonce") != theirs);
        // Saying a phone's id ends nobody's link; proving it does. The newer link replaces the older one.
        CHECK(!p->closed && b.agent.sessions() == 4);
        CHECK(Parsed(b.call(q, "auth", "{\"proof\":\"" + Bench::proof(q, Bench::keyOf("1"), "nbt-auth-2", kNonce, theirs) + "\"}")).result.isObject());
        b.settle();
        CHECK(p->closed && !q->closed && !r->closed && b.agent.sessions() == 3);
        CHECK(closingReason(p) == "replaced" && q->events.empty() && r->events.empty());
    }
    {   // A wrong proof: of another key, of another hello, of the wrong label. Each ends the link.
        b.pair("2");
        for (int i = 0; i < 4; ++i) {
            Phone* p = b.greeted("2", 4096, "serial");
            Parsed first(b.call(p, "auth", nonceParams()));
            std::string theirs = str(first.result, "nonce"), key = Bench::keyOf("2");
            const char* label = "nbt-auth-2";
            if (i == 0) key = Bench::keyOf("1");
            if (i == 1) p->helloSent[p->helloSent.size() - 3] ^= 1;     // a hello that asked for another frame size
            if (i == 2) p->helloReply[p->helloReply.size() - 3] ^= 1;   // an answer that gave another
            if (i == 3) label = "nbt-auth-1";                           // the agent's own proof, sent back
            CHECK_IN(num(i), Parsed(b.call(p, "auth", "{\"proof\":\"" + Bench::proof(p, key, label, kNonce, theirs) + "\"}")).code() == "auth-failed");
            CHECK_IN(num(i), p->closed);
        }
        // A key that was replaced between the two calls is a wrong one, though the proof fitted when it was asked for.
        Phone* p = b.greeted("2", 4096, "serial");
        std::string theirs = str(Parsed(b.call(p, "auth", nonceParams())).result, "nonce");
        b.pair("2", std::string(kPairingKeyBytes, 'x'));
        CHECK(Parsed(b.call(p, "auth", "{\"proof\":\"" + Bench::proof(p, Bench::keyOf("2"), "nbt-auth-2", kNonce, theirs) + "\"}")).code() == "auth-failed");
        Phone* again = b.greeted("2", 4096, "serial");
        CHECK(Parsed(b.authenticate(again, std::string(kPairingKeyBytes, 'x'))).result.isObject());
    }
    {   // Only a phone has a key; the bench's TCP link is held to the same as a serial one; the local socket has
        // no auth at all.
        Phone* cli = b.connect("serial");
        CHECK(Parsed(b.hello(cli, kGoodHello)).result.isObject());
        CHECK(Parsed(b.call(cli, "info.get", "{}")).code() == "unauthenticated");
        CHECK(Parsed(b.call(cli, "auth", nonceParams())).code() == "auth-failed" && cli->closed);
        Phone* tcp = b.greeted("1", 65536, "tcp");
        CHECK(integer(Parsed(tcp->helloReply).result, "maxFrame") == 65536);
        CHECK(Parsed(b.call(tcp, "info.get", "{}")).code() == "unauthenticated");
        CHECK(Parsed(b.authenticate(tcp, Bench::keyOf("1"))).result.isObject());
        CHECK(Parsed(b.call(tcp, "info.get", "{}")).result.isObject());
        Phone* local = b.phone("1");
        CHECK(Parsed(b.call(local, "auth", nonceParams())).code() == "forbidden" && !local->closed);
        // A phone on the local socket and the same phone on its own link are told apart by nothing but the id,
        // and the newer of the two holds.
        CHECK(tcp->closed && closingReason(tcp) == "replaced");
    }
    {   // A head unit that cannot give random bytes makes no nonce, and says so; the link stays for the next try.
        Phone* p = b.greeted("1", 4096, "serial");
        b.random.noSecret = true;
        CHECK(Parsed(b.call(p, "auth", nonceParams())).code() == "internal" && !p->closed);
        b.random.noSecret = false;
        CHECK(Parsed(b.authenticate(p, Bench::keyOf("1"))).result.isObject());
    }
}

// ---- pairing: the window, the code, the key ------------------------------------------------------------------------

static std::string codeParams(const std::string& code) { return "{\"code\":\"" + code + "\"}"; }

// A code that is not this one.
static std::string wrongCode(std::string code) {
    code[code.size() - 1] = code[code.size() - 1] == '9' ? '0' : (char)(code[code.size() - 1] + 1);
    return code;
}

// The reason of the last pairing.closed a controller was sent, or "" if it was sent none; the event is taken away.
static std::string pairingClosed(Phone* controller) {
    std::string text = lastEvent(controller, "pairing.closed");
    controller->events.clear();
    return text.empty() ? text : str(Parsed(text).params, "reason");
}

static void testPairing() {
    Bench b("pairing");
    std::string logPath = b.root + "/log.txt";
    FILE* log = fopen(logPath.c_str(), "w");
    CHECK(log != 0);
    b.agent.setLog(log);
    std::vector<std::string> secrets;   // every code and key of this test: none of them may be in the log

    Phone* ctl = b.connect();
    CHECK(Parsed(b.hello(ctl, kGoodHello)).result.isObject());
    Phone* p = b.greeted("1", 4096, "serial");

    // No window: no key. And the window is opened from the head unit only, whoever asks.
    CHECK(Parsed(b.call(p, "pairing.issue", codeParams("123456"))).code() == "forbidden");
    CHECK(Parsed(b.call(p, "pairing.open", "{}")).code() == "unauthenticated");
    Phone* paired = b.phone("8", 4096, "serial");
    CHECK(Parsed(b.call(paired, "pairing.open", "{}")).code() == "forbidden");
    CHECK(Parsed(b.call(paired, "pairing.close", "{}")).code() == "forbidden");

    // The window: a code of six digits, good for 120 s.
    Parsed opened(b.call(ctl, "pairing.open", "{}"));
    std::string code = str(opened.result, "code");
    secrets.push_back(code);
    CHECK_IN(code, code.size() == kPairingCodeDigits && code.find_first_not_of("0123456789") == std::string::npos);
    CHECK(integer(opened.result, "expiresIn") == kPairingWindowSeconds && opened.result.size() == 2);
    // What is not a code at all is refused as such, and is not a guess.
    const char* bad[] = {"{}", "{\"code\":123456}", "{\"code\":\"12345\"}", "{\"code\":\"1234567\"}", "{\"code\":\"12345a\"}", "{\"code\":\" 23456\"}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK_IN(bad[i], Parsed(b.call(p, "pairing.issue", bad[i])).code() == "bad-request");
    // Not for the local socket, and not for a controller that is no phone.
    CHECK(Parsed(b.call(ctl, "pairing.issue", codeParams(code))).code() == "forbidden");
    Phone* cli = b.connect("serial");
    CHECK(Parsed(b.hello(cli, kGoodHello)).result.isObject());
    CHECK(Parsed(b.call(cli, "pairing.issue", codeParams(code))).code() == "forbidden");
    // Two wrong codes, and the right one: a key, filed under the phone's id, and the controller is told.
    CHECK(Parsed(b.call(p, "pairing.issue", codeParams(wrongCode(code)))).code() == "rejected");
    CHECK(Parsed(b.call(p, "pairing.issue", codeParams(wrongCode(wrongCode(code))))).code() == "rejected");
    CHECK(pairingClosed(ctl).empty() && b.agent.phones().find(std::string(32, '1')) == 0);
    Parsed issued(b.call(p, "pairing.issue", codeParams(code)));
    std::string keyHex = str(issued.result, "key");
    secrets.push_back(keyHex);
    uint8_t keyBytes[kPairingKeyBytes];
    CHECK_IN(keyHex, fromHex(keyHex, keyBytes, sizeof keyBytes) && issued.result.size() == 1);
    std::string key((const char*)keyBytes, sizeof keyBytes);
    CHECK(pairingClosed(ctl) == "issued");
    const PairedPhone* record = b.agent.phones().find(std::string(32, '1'));
    CHECK(record != 0 && record->name == "Test phone" && record->pairedAt == FakeClock::kPairedAt &&
          memcmp(record->key, keyBytes, sizeof keyBytes) == 0);
    struct stat st;
    CHECK(stat((b.platform.stateDir() + "/phones.json").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    // The window closed with that: the code is used up. The key is what auth takes, on the same link.
    CHECK(Parsed(b.call(p, "pairing.issue", codeParams(code))).code() == "forbidden");
    CHECK(Parsed(b.call(p, "info.get", "{}")).code() == "unauthenticated");
    CHECK(Parsed(b.authenticate(p, key)).result.isObject());
    CHECK(Parsed(b.call(p, "info.get", "{}")).result.isObject());

    // A phone that pairs again gets a new key, and the old one is worth nothing.
    code = str(Parsed(b.call(ctl, "pairing.open", "{}")).result, "code");
    secrets.push_back(code);
    Phone* again = b.greeted("1", 4096, "serial");
    std::string newHex = str(Parsed(b.call(again, "pairing.issue", codeParams(code))).result, "key");
    secrets.push_back(newHex);
    CHECK(newHex.size() == 64 && newHex != keyHex && pairingClosed(ctl) == "issued" && b.agent.phones().all().size() == 2);
    std::string theirs = str(Parsed(b.call(again, "auth", nonceParams())).result, "nonce");
    CHECK(Parsed(b.call(again, "auth", "{\"proof\":\"" + Bench::proof(again, key, "nbt-auth-2", kNonce, theirs) + "\"}")).code() == "auth-failed");
    CHECK(again->closed);

    // Three wrong codes close the window, and for 30 s there is no new one.
    code = str(Parsed(b.call(ctl, "pairing.open", "{}")).result, "code");
    secrets.push_back(code);
    Phone* guesser = b.greeted("2", 4096, "serial");
    for (unsigned i = 0; i < kPairingTries; ++i) {
        CHECK(pairingClosed(ctl).empty());
        CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(wrongCode(code)))).code() == "rejected");
    }
    CHECK(pairingClosed(ctl) == "wrong-code");
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(code))).code() == "forbidden" && !guesser->closed);
    CHECK(Parsed(b.call(ctl, "pairing.open", "{}")).code() == "try-later");
    b.clock.now += kPairingPauseSeconds * 1000 - 1;
    CHECK(Parsed(b.call(ctl, "pairing.open", "{}")).code() == "try-later");
    b.clock.now += 1;
    code = str(Parsed(b.call(ctl, "pairing.open", "{}")).result, "code");
    secrets.push_back(code);
    // A new window has its own three tries.
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(wrongCode(code)))).code() == "rejected");
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(wrongCode(code)))).code() == "rejected");

    // The code runs out after 120 s, to the millisecond, and the controller is told.
    b.clock.now += kPairingWindowSeconds * 1000 - 1;
    b.settle();
    CHECK(pairingClosed(ctl).empty());
    b.clock.now += 1;
    b.settle();
    CHECK(pairingClosed(ctl) == "expired");
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(code))).code() == "forbidden");

    // pairing.close: no event, and no window. Another controller's window is not its to close.
    Phone* other = b.connect();
    CHECK(Parsed(b.hello(other, kGoodHello)).result.isObject());
    code = str(Parsed(b.call(ctl, "pairing.open", "{}")).result, "code");
    secrets.push_back(code);
    CHECK(Parsed(b.call(other, "pairing.close", "{}")).result.isObject());
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(wrongCode(code)))).code() == "rejected");
    CHECK(Parsed(b.call(ctl, "pairing.close", "{}")).result.size() == 0);
    CHECK(Parsed(b.call(ctl, "pairing.close", "{}")).result.isObject());   // twice is harmless
    CHECK(pairingClosed(ctl).empty());
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(code))).code() == "forbidden");

    // One window: a controller that opens another gets a new code and hears nothing; a second controller takes
    // the window over, and the first is told that its code is void.
    std::string first = str(Parsed(b.call(ctl, "pairing.open", "{}")).result, "code");
    std::string second = str(Parsed(b.call(ctl, "pairing.open", "{}")).result, "code");
    std::string third = str(Parsed(b.call(other, "pairing.open", "{}")).result, "code");
    secrets.push_back(first);
    secrets.push_back(second);
    secrets.push_back(third);
    CHECK(first != second && second != third && pairingClosed(ctl) == "replaced" && pairingClosed(other).empty());
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(second))).code() == "rejected");
    // The window goes with the link of the controller that shows its code.
    other->hangUp();
    b.settle();
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(third))).code() == "forbidden");

    // No random bytes: no code, and no key. The window stays for the phone's next try.
    b.random.noSecret = true;
    CHECK(Parsed(b.call(ctl, "pairing.open", "{}")).code() == "internal");
    b.random.noSecret = false;
    code = str(Parsed(b.call(ctl, "pairing.open", "{}")).result, "code");
    secrets.push_back(code);
    b.random.noSecret = true;
    CHECK(Parsed(b.call(guesser, "pairing.issue", codeParams(code))).code() == "internal" && pairingClosed(ctl).empty());
    b.random.noSecret = false;
    std::string guesserHex = str(Parsed(b.call(guesser, "pairing.issue", codeParams(code))).result, "key");
    secrets.push_back(guesserHex);
    CHECK(guesserHex.size() == 64 && pairingClosed(ctl) == "issued");

    // The log says that all of this happened, and holds no code and no key.
    b.agent.setLog(0);
    fclose(log);
    std::string text = testutil::readFile(logPath);
    CHECK(has(text, "the pairing window is open for 120 s") && has(text, "the pairing window closed: issued") &&
          has(text, "the pairing window closed: wrong-code") && has(text, "the pairing window closed: expired") &&
          has(text, "paired the phone called \"Test phone\", id 11111111111111111111111111111111") &&
          has(text, "pairing.issue -> rejected") && has(text, "wrong pairing code, 3 of 3"));
    for (size_t i = 0; i < secrets.size(); ++i) CHECK_IN(secrets[i], secrets[i].size() >= 6 && !has(text, secrets[i]));

    // The pairings outlast the agent.
    {
        Bench restarted("pairing", true);
        CHECK(restarted.agent.phones().all().size() == 3);
        uint8_t raw[kPairingKeyBytes];
        CHECK(fromHex(newHex, raw, sizeof raw));
        Phone* back = restarted.greeted("1", 4096, "serial");
        CHECK(Parsed(restarted.authenticate(back, std::string((const char*)raw, sizeof raw))).result.isObject());
        // A record that cannot be read: the agent starts, and no phone is paired.
        std::string why;
        CHECK_IN(why, fs::writeFile(restarted.platform.stateDir() + "/phones.json", "{\"version\":1,\"phones\":[{}]}\n", 0600, &why));
    }
    Bench damaged("pairing", true);
    CHECK(damaged.agent.phones().all().empty() && damaged.agent.headUnitId() == kHeadUnitId);
    Phone* nobody = damaged.greeted("1", 4096, "serial");
    CHECK(Parsed(damaged.call(nobody, "auth", nonceParams())).code() == "auth-failed");
}

// ---- the paired phones: listed and forgotten, from the head unit --------------------------------------------------

static void testPhones() {
    Bench b("phones");
    Phone* ctl = b.connect();
    CHECK(Parsed(b.hello(ctl, kGoodHello)).result.isObject());
    CHECK(Parsed(b.call(ctl, "phones.list", "{}")).result.find("phones").size() == 0);
    Phone* one = b.phone("1", 4096, "serial");
    Phone* two = b.phone("2", 4096, "serial");
    Phone* local = b.phone("1");   // the same id on the local socket, where it needs no pairing
    b.settle();
    CHECK(one->closed);            // and where, as the newer link of that phone, it took the older one's place
    one = b.phone("1", 4096, "serial");
    b.settle();
    CHECK(local->closed && !two->closed);

    // The list: oldest pairing first, and never a key.
    std::string text = b.call(ctl, "phones.list", "{}");
    Parsed list(text);
    json::Value phones = list.result.find("phones");
    CHECK_IN(text, phones.size() == 2 && str(phones.at(0), "phoneId") == std::string(32, '2') && str(phones.at(1), "phoneId") == std::string(32, '1'));
    CHECK_IN(text, str(phones.at(0), "name") == "Test phone" && integer(phones.at(0), "pairedAt") == FakeClock::kPairedAt &&
                       phones.at(0).size() == 3 && !has(text, "key"));
    // Both are the head unit's own business.
    CHECK(Parsed(b.call(two, "phones.list", "{}")).code() == "forbidden");
    CHECK(Parsed(b.call(two, "phones.forget", "{\"all\":true}")).code() == "forbidden" && !two->closed);

    const char* bad[] = {"{}", "{\"phoneId\":\"1\"}", "{\"phoneId\":7}", "{\"all\":false}", "{\"all\":\"yes\"}",
                         "{\"phoneId\":\"11111111111111111111111111111111\",\"all\":true}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK_IN(bad[i], Parsed(b.call(ctl, "phones.forget", bad[i])).code() == "bad-request");
    CHECK(Parsed(b.call(ctl, "phones.forget", "{\"phoneId\":\"" + std::string(32, '3') + "\"}")).code() == "not-found");
    CHECK(b.agent.phones().all().size() == 2);

    // One phone forgotten: its link ends, its key is worth nothing, and the other phone is left alone.
    CHECK(integer(Parsed(b.call(ctl, "phones.forget", "{\"phoneId\":\"" + std::string(32, '1') + "\"}")).result, "forgotten") == 1);
    CHECK(one->closed && !two->closed && b.agent.phones().find(std::string(32, '1')) == 0);
    CHECK(closingReason(one) == "forgotten" && two->events.empty());
    Phone* back = b.greeted("1", 4096, "serial");
    CHECK(Parsed(b.call(back, "auth", nonceParams())).code() == "auth-failed");
    CHECK(Parsed(b.call(two, "info.get", "{}")).result.isObject());

    // Every phone forgotten. A phone on the local socket is not there by a pairing, and stays.
    b.pair("1");
    b.pair("4");
    local = b.phone("2");
    b.settle();
    CHECK(two->closed);
    two = b.phone("2", 4096, "serial");
    Phone* onLocal = b.phone("5");
    CHECK(integer(Parsed(b.call(ctl, "phones.forget", "{\"all\":true}")).result, "forgotten") == 3);
    CHECK(two->closed && !onLocal->closed && b.agent.phones().all().empty());
    CHECK(closingReason(two) == "forgotten" && onLocal->events.empty() && back->events.empty());
    CHECK(integer(Parsed(b.call(ctl, "phones.forget", "{\"all\":true}")).result, "forgotten") == 0);
    // And it is forgotten for the next agent too.
    Bench restarted("phones", true);
    CHECK(restarted.agent.phones().all().empty());
}

// ---- link.closing: how the agent ends a link on purpose -----------------------------------------------------------

static void testLinkClosing() {
    {   // The agent stops, or ends for a newer version of itself: every link but the local socket's is told and
        // closed, and the call comes back with that done.
        const char* reasons[] = {kClosingStopping, kClosingRestarting};
        for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; ++i) {
            Bench b("closing-all");
            Phone* local = b.phone("1");
            Phone* paired = b.phone("2", 4096, "serial");
            Phone* greeted = b.greeted("3", 4096, "serial");   // in the middle of being paired, say
            Phone* bench = b.phone("4", 65536, "tcp");
            Phone* wordless = b.connect("serial");             // the magic bytes, and no hello yet
            b.settle();
            CHECK(b.agent.sessions() == 5);
            b.agent.endPhoneLinks(reasons[i], 2000);
            CHECK(b.agent.sessions() == 1);
            for (size_t k = 0; k < b.phones.size(); ++k) b.phones[k]->pump();
            CHECK_IN(reasons[i], paired->closed && closingReason(paired) == reasons[i]);
            CHECK_IN(reasons[i], greeted->closed && closingReason(greeted) == reasons[i]);
            CHECK_IN(reasons[i], bench->closed && closingReason(bench) == reasons[i]);
            // A link on which hello was never done is closed without a word, and the local socket's is left.
            CHECK(wordless->closed && wordless->lastHeard.empty() && !local->closed && local->events.empty());
            CHECK(Parsed(b.call(local, "info.get", "{}")).result.isObject());
            b.agent.endPhoneLinks(reasons[i], 2000);   // with nothing left to end, it does nothing
            CHECK(b.agent.sessions() == 1 && !local->closed);
        }
    }
    {   // Nothing follows the event, and nothing the phone sends after it is read: a request that crossed it gets
        // no answer.
        Bench b("closing-last");
        Phone* ctl = b.connect();
        CHECK(Parsed(b.hello(ctl, kGoodHello)).result.isObject());
        Phone* p = b.phone("1", 4096, "serial");
        int64_t forget = ctl->request("phones.forget", "{\"all\":true}");   // the agent serves the older link first
        int64_t crossed = p->request("info.get", "{}");
        b.settle();
        CHECK(integer(Parsed(ctl->reply(forget)).result, "forgotten") == 1);
        CHECK(p->closed && p->replies.count(crossed) == 0 && closingReason(p) == "forgotten" && b.agent.sessions() == 1);
    }
    {   // A link that does not take the event is closed all the same, two seconds later.
        Bench b("closing-stuck");
        Phone* ctl = b.connect();
        CHECK(Parsed(b.hello(ctl, kGoodHello)).result.isObject());
        Phone* p = b.phone("1", 4096, "serial");
        p->deaf = true;   // it asks and asks, and reads nothing: what the agent answers fills the link
        for (int i = 0; i < 6000; ++i) p->request("info.get", "{}");
        b.settle();
        CHECK(integer(Parsed(b.call(ctl, "phones.forget", "{\"all\":true}")).result, "forgotten") == 1);
        CHECK(b.agent.sessions() == 2);   // told, and the event is still on its way
        b.clock.now += 1999;
        b.settle();
        CHECK(b.agent.sessions() == 2);
        b.clock.now += 1;
        b.settle();
        CHECK(b.agent.sessions() == 1);
        p->deaf = false;
        p->pump();
        CHECK(p->closed && !p->replies.empty() && p->events.empty());   // what the link held, and not the event
        // The same at the end of the process, where the clock is not what is waited on.
        Phone* q = b.phone("2", 4096, "serial");
        q->deaf = true;
        for (int i = 0; i < 6000; ++i) q->request("info.get", "{}");
        b.settle();
        b.agent.endPhoneLinks(kClosingStopping, 100);
        CHECK(b.agent.sessions() == 1);
        q->deaf = false;
        q->pump();
        CHECK(q->closed && q->events.empty());
    }
    {   // The log says what a link was told, and why it ended.
        Bench b("closing-log");
        std::string logPath = b.root + "/log.txt";
        FILE* log = fopen(logPath.c_str(), "w");
        CHECK(log != 0);
        b.agent.setLog(log);
        b.phone("1", 4096, "serial");
        b.agent.endPhoneLinks(kClosingRestarting, 2000);
        b.agent.setLog(0);
        fclose(log);
        std::string text = testutil::readFile(logPath);
        CHECK_IN(text, has(text, ": told link.closing, restarting") && has(text, " ended: the agent is restarting"));
    }
}

// ---- info, packages, sources --------------------------------------------------------------------------------------

static void testInfo() {
    Bench b("info");
    b.platform.fakeFree = 123456789;
    Phone* p = b.phone();
    Parsed info(b.call(p, "info.get", "{}"));
    CHECK(str(info.result, "agentVersion") == "1.0.0" && str(info.result, "headUnitId") == kHeadUnitId);
    CHECK(integer(info.result, "manifestVersion") == 1 && str(info.result, "restartNeeded") == "none");
    json::Value free = info.result.find("free");
    CHECK(free.size() == 4 && integer(free, "app") == 123456789 && integer(free, "data") == 123456789 &&
          integer(free, "plugins") == 123456789 && integer(free, "staging") == 123456789);

    Parsed sources(b.call(p, "sources.list", "{}"));
    json::Value list = sources.result.find("sources");
    CHECK(list.size() == 1 && str(list.at(0), "key") == toHex(gPackageKey.key, 32) && str(list.at(0), "name") == "test package key");
    CHECK(list.at(0).find("embedded").isBool() && list.at(0).find("embedded").boolean());

    std::string trusted = toHex(gPackageKey.key, 32), unknown = toHex(gOtherKey.key, 32);
    CHECK(Parsed(b.call(p, "source.add", "{\"name\":\"Mine\",\"key\":\"" + trusted + "\"}")).result.isObject());
    CHECK(Parsed(b.call(p, "source.add", "{\"name\":\"Mine\",\"key\":\"" + unknown + "\"}")).code() == "unsupported");
    CHECK(Parsed(b.call(p, "source.add", "{\"name\":\"\",\"key\":\"" + unknown + "\"}")).code() == "bad-request");
    CHECK(Parsed(b.call(p, "source.add", "{\"name\":\"Mine\",\"key\":\"abc\"}")).code() == "bad-request");
    CHECK(Parsed(b.call(p, "source.remove", "{\"key\":\"" + trusted + "\"}")).code() == "forbidden");
    CHECK(Parsed(b.call(p, "source.remove", "{\"key\":\"" + unknown + "\"}")).code() == "not-found");
    CHECK(Parsed(b.call(p, "source.remove", "{}")).code() == "bad-request");

    Parsed none(b.call(p, "packages.list", "{}"));
    CHECK(none.result.find("packages").size() == 0 && !none.result.find("more").boolean());
    CHECK(Parsed(b.call(p, "packages.list", "{\"after\":7}")).code() == "bad-request");
    CHECK(Parsed(b.call(p, "job.status", "{}")).result.find("events").size() == 0);
    CHECK(!Parsed(b.call(p, "job.status", "{}")).result.find("job").present());
    CHECK(Parsed(b.call(p, "job.status", "{\"planId\":\"0123456789abcdef\"}")).code() == "not-found");
    CHECK(Parsed(b.call(p, "job.status", "{\"planId\":\"nope\"}")).code() == "bad-request");

    install(b, p, std::string(kHello) + " org.nbtapp.demoplugin-1.0.0", std::string(kHelloAndRuntime) + " org.nbtapp.demoplugin-1.0.0");
    Parsed all(b.call(p, "packages.list", "{}"));
    json::Value packages = all.result.find("packages");
    CHECK(packages.size() == 3 && !all.result.find("more").boolean());
    CHECK(str(packages.at(0), "id") == "org.nbtapp.demoplugin" && str(packages.at(1), "id") == "org.nbtapp.hello" &&
          str(packages.at(2), "id") == "org.nbtapp.runtime");
    CHECK(str(packages.at(1), "name") == "Hello NBT" && str(packages.at(1), "version") == "1.2.0" && str(packages.at(1), "key") == trusted);
    CHECK(packages.at(1).find("flags").size() == 1 && packages.at(1).find("flags").at(0).string() == "runs-scripts");
    CHECK(packages.at(2).find("flags").size() == 0);
    Parsed tail(b.call(p, "packages.list", "{\"after\":\"org.nbtapp.hello\"}"));
    CHECK(tail.result.find("packages").size() == 1 && str(tail.result.find("packages").at(0), "id") == "org.nbtapp.runtime");
    // The plugin asks for an HMI restart, and the head unit says so until it has had one.
    CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "hmi-restart");

    // On a link with small frames a page ends when the frame is full, and `after` gets the rest.
    std::string why;
    CHECK(fs::makeDirs(b.platform.systemPath("/opt/demo"), &why));
    CHECK(fs::writeFile(b.platform.systemPath("/opt/demo/menu.xml"), "stock menu, firmware A\n", 0644, &why));
    std::string more = "org.nbtapp.menupatch-1.0.0 org.nbtapp.rival2-1.0.0 org.nbtapp.longname-1.0.0";
    install(b, p, more, more);
    Phone* small = b.phone("2", 1024);
    std::string after, seen;
    int pages = 0;
    for (;;) {
        Parsed page(b.call(small, "packages.list", after.empty() ? std::string("{}") : "{\"after\":\"" + after + "\"}"));
        json::Value list = page.result.find("packages");
        CHECK(list.size() >= 1);
        for (size_t i = 0; i < list.size(); ++i) {
            after = str(list.at(i), "id");
            seen += after + " ";
        }
        ++pages;
        if (!page.result.find("more").boolean()) break;
    }
    CHECK_IN(seen + num(pages), seen == "org.nbtapp.demoplugin org.nbtapp.hello org.nbtapp.longname org.nbtapp.menupatch org.nbtapp.rival2 org.nbtapp.runtime " && pages == 2);
}

// ---- job.plan -----------------------------------------------------------------------------------------------------

static void testPlan() {
    Bench b("plan");
    Phone* p = b.phone();

    const char* bad[] = {
        "{}",
        "{\"install\":[],\"remove\":[]}",
        "{\"install\":{}}",
        "{\"install\":[{\"id\":\"org.nbtapp.hello\"}]}",
        "{\"install\":[{\"id\":\"Hello\",\"version\":\"1.0.0\"}]}",
        "{\"install\":[{\"id\":\"org.nbtapp.hello\",\"version\":\">=1.0.0\"}]}",
        "{\"install\":[{\"id\":\"org.nbtapp.hello\",\"version\":\"1.0.0\"},{\"id\":\"org.nbtapp.hello\",\"version\":\"1.1.0\"}]}",
        "{\"install\":[{\"id\":\"org.nbtapp.hello\",\"version\":\"1.0.0\"}],\"remove\":[{\"id\":\"org.nbtapp.hello\"}]}",
        "{\"remove\":[{\"id\":\"org.nbtapp.hello\",\"deleteData\":1}]}",
        "{\"remove\":[{\"id\":\"org.nbtapp.hello\"}],\"offers\":[{\"id\":\"org.nbtapp.hello\"}]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK_IN(bad[i], Parsed(b.call(p, "job.plan", bad[i])).code() == "bad-request");
    std::string badOffer = planParams(kHello, "", kHello);
    badOffer.replace(badOffer.find("\"size\":"), 7, "\"size\":0,\"was\":");
    CHECK(Parsed(b.call(p, "job.plan", badOffer)).code() == "bad-request");

    // A target with no offer; a dependency with no offer.
    planRefused(b, p, planParams(kHello, "", kRuntime), "no-offer", "org.nbtapp.hello");
    {
        std::string text = b.call(p, "job.plan", planParams(kHello, "", kHello));
        Parsed r(text);
        json::Value missing = r.result.find("missing");
        CHECK_IN(text, !r.result.find("planId").present() && r.result.find("problems").size() == 0 && missing.size() == 1);
        CHECK(str(missing.at(0), "id") == "org.nbtapp.runtime" && str(missing.at(0), "version") == ">=2.0.0 <3.0.0" &&
              str(missing.at(0), "requiredBy") == "org.nbtapp.hello");
    }

    // The plan for hello: its dependency comes from the offers and goes first. Of the two runtimes on offer the
    // one in range is taken, although the other is higher.
    p->fetches.clear();
    std::string text = b.call(p, "job.plan", planParams(kHello, "", std::string(kHelloAndRuntime) + " org.nbtapp.runtime-3.0.0 " + kHelloOld));
    Parsed plan(text);
    json::Value steps = plan.result.find("steps");
    CHECK_IN(text, str(plan.result, "planId").size() == 16 && steps.size() == 2);
    CHECK(str(steps.at(0), "action") == "install" && str(steps.at(0), "id") == "org.nbtapp.runtime" && str(steps.at(0), "version") == "2.1.0" &&
          str(steps.at(0), "sha256") == pkg(kRuntime).sha256);
    CHECK(str(steps.at(1), "action") == "install" && str(steps.at(1), "id") == "org.nbtapp.hello" && !steps.at(1).find("oldVersion").present());
    CHECK(plan.result.find("flags").size() == 1 && plan.result.find("flags").at(0).string() == "runs-scripts");
    CHECK(integer(plan.result, "fetchBytes") == (long long)(pkg(kHello).size + pkg(kRuntime).size));
    CHECK(integer(plan.result, "installBytes") > 0 && str(plan.result, "restart") == "none");
    // Planning pulled the two manifests, whole, and no archive; and it changed nothing.
    CHECK(p->fetches.size() == 2 && p->fetches[0] == pkg(kHello).manifestSha256 + " 0 -" && p->fetches[1] == pkg(kRuntime).manifestSha256 + " 0 -");
    CHECK(!b.installed("org.nbtapp.hello") && !b.agent.job().exists());

    // What can be wrong with an offered manifest.
    {
        Pkg forged = pkg(kRuntime);   // a signature that is not this manifest's
        forged.signature = pkg(kHello).signature;
        std::string params = "{\"install\":[{\"id\":\"org.nbtapp.runtime\",\"version\":\"2.1.0\"}],\"offers\":[" + offer(forged) + "]}";
        planRefused(b, p, params, "bad-signature", "org.nbtapp.runtime");

        Pkg renamed = pkg(kHelloOld);   // offered as 1.2.0; the manifest says 1.1.0
        renamed.version = "1.2.0";
        params = "{\"install\":[{\"id\":\"org.nbtapp.hello\",\"version\":\"1.2.0\"}],\"offers\":[" + offer(renamed) + "," + offer(pkg(kRuntime)) + "]}";
        planRefused(b, p, params, "offer-mismatch", "org.nbtapp.hello");

        std::string keep = p->blobs[pkg(kRuntime).manifestSha256];
        p->blobs.erase(pkg(kRuntime).manifestSha256);   // the phone does not have it
        planRefused(b, p, planParams(kRuntime, "", kRuntime), "fetch-failed");
        p->blobs[pkg(kRuntime).manifestSha256] = keep + " ";   // other bytes than the hash names
        planRefused(b, p, planParams(kRuntime, "", kRuntime), "fetch-failed");
        p->blobs[pkg(kRuntime).manifestSha256] = std::string((1u << 20) + 1, 'x');   // too large to be a manifest
        planRefused(b, p, planParams(kRuntime, "", kRuntime), "bad-manifest");
        CHECK(!p->closed && !p->streaming());
        p->blobs[pkg(kRuntime).manifestSha256] = keep;
        planOk(b, p, kRuntime, "", kRuntime);
    }
    {
        Pkg unsignedOffer = pkg(kRuntime);   // a signature no key made
        unsignedOffer.signature = std::string(128, '0');
        planRefused(b, p, "{\"install\":[{\"id\":\"org.nbtapp.runtime\",\"version\":\"2.1.0\"}],\"offers\":[" + offer(unsignedOffer) + "]}", "bad-signature");
    }

    // Against what is installed.
    planRefused(b, p, planParams("", "org.nbtapp.hello", ""), "not-installed", "org.nbtapp.hello");
    install(b, p, kHello, kHelloAndRuntime);
    planRefused(b, p, planParams(kHello, "", kHelloAndRuntime), "already-installed");
    planRefused(b, p, planParams(kHelloOld, "", std::string(kHelloOld) + " " + kRuntime), "downgrade");
    planRefused(b, p, planParams("", "org.nbtapp.runtime", ""), "required-by", "org.nbtapp.runtime");
    planRefused(b, p, planParams("org.nbtapp.rival-1.0.0", "", "org.nbtapp.rival-1.0.0"), "conflict", "org.nbtapp.rival");
    // Two packages that declare the same serial service, in one job: the second step is the one refused.
    {
        const char* radios = "org.nbtapp.radio-1.0.0 org.nbtapp.radio2-1.0.0";
        planRefused(b, p, planParams(radios, "", radios), "conflict", "org.nbtapp.radio2");
        planOk(b, p, "org.nbtapp.radio-1.0.0", "", "org.nbtapp.radio-1.0.0");
    }
    // An upgrade that would take the runtime out of the range the installed hello needs.
    planRefused(b, p, planParams("org.nbtapp.runtime-3.0.0", "", "org.nbtapp.runtime-3.0.0"), "conflict", "org.nbtapp.runtime");
    // Removing both is fine, whichever is named first: the dependant goes first.
    {
        std::string both = b.call(p, "job.plan", planParams("", "org.nbtapp.runtime org.nbtapp.hello!", ""));
        Parsed r(both);
        json::Value s = r.result.find("steps");
        CHECK_IN(both, r.result.find("planId").isString() && s.size() == 2 && str(s.at(0), "id") == "org.nbtapp.hello" &&
                           str(s.at(0), "action") == "remove" && str(s.at(0), "version") == "1.2.0" && str(s.at(1), "id") == "org.nbtapp.runtime");
        CHECK(integer(r.result, "fetchBytes") == 0 && !s.at(0).find("sha256").present());
    }
    // Removing hello lets rival in, in one job: removals come first.
    {
        std::string swap = b.call(p, "job.plan", planParams("org.nbtapp.rival-1.0.0", "org.nbtapp.hello", "org.nbtapp.rival-1.0.0"));
        Parsed r(swap);
        json::Value s = r.result.find("steps");
        CHECK_IN(swap, r.result.find("planId").isString() && s.size() == 2 && str(s.at(0), "action") == "remove" && str(s.at(1), "id") == "org.nbtapp.rival");
        CHECK(r.result.find("flags").size() == 2);
    }
    // Removing a package that a package of the same job needs.
    planRefused(b, p, planParams("org.nbtapp.hello-1.3.0", "org.nbtapp.runtime", "org.nbtapp.hello-1.3.0"), "required-by", "org.nbtapp.runtime");

    // Files: a path another package owns, and a stock file that is not there. Both are seen without the archive.
    Bench f("plan-files");
    Phone* q = f.phone();
    install(f, q, "org.nbtapp.rival-1.0.0", "org.nbtapp.rival-1.0.0");
    planRefused(f, q, planParams("org.nbtapp.rival2-1.0.0", "", "org.nbtapp.rival2-1.0.0"), "file-owned", "org.nbtapp.rival2");
    planRefused(f, q, planParams("org.nbtapp.menupatch-1.0.0", "", "org.nbtapp.menupatch-1.0.0"), "stock-mismatch", "org.nbtapp.menupatch");
    // In one job with the removal of the owner, the path is free by the time the step runs.
    planOk(f, q, "org.nbtapp.rival2-1.0.0", "org.nbtapp.rival", "org.nbtapp.rival2-1.0.0");
    // Two packages of one job that want one path.
    {
        Bench g("plan-files-2");
        Phone* r = g.phone();
        planRefused(g, r, planParams("org.nbtapp.rival-1.0.0 org.nbtapp.rival2-1.0.0", "", "org.nbtapp.rival-1.0.0 org.nbtapp.rival2-1.0.0"),
                    "file-owned", "org.nbtapp.rival2");
    }
    // 64 KiB are kept free. With 66,000 bytes there is room for the package's 93 bytes where they go, but not for
    // its archive and its unpacked files in staging as well.
    f.platform.fakeFree = 66000;
    planRefused(f, q, planParams(kRuntime, "", kRuntime), "no-space", "org.nbtapp.runtime");
    f.platform.fakeFree = 67000;
    planOk(f, q, kRuntime, "", kRuntime);
    // To the byte: here staging and the package's folders are on one file system, so the archive, its unpacked
    // files and the files where they go are counted together.
    {
        const Pkg& runtime = pkg(kRuntime);
        json::Document manifest;
        std::string why;
        CHECK_IN(why, manifest.parse(runtime.manifest, &why));
        json::Value contents = manifest.root().find("contents");
        long long unpacked = 0;
        for (size_t i = 0; i < contents.size(); ++i) unpacked += integer(contents.at(i), "size");
        Parsed ok(f.call(q, "job.plan", planParams(kRuntime, "", kRuntime)));
        long long placed = integer(ok.result, "installBytes");
        CHECK(unpacked > 0 && placed > 0 && integer(ok.result, "fetchBytes") == (long long)runtime.size);
        long long needed = (long long)runtime.size + unpacked + placed;
        f.platform.fakeFree = (64 << 10) + needed - 1;
        planRefused(f, q, planParams(kRuntime, "", kRuntime), "no-space", "org.nbtapp.runtime");
        f.platform.fakeFree = (64 << 10) + needed;
        planOk(f, q, kRuntime, "", kRuntime);
    }
    // A platform that keeps more free where staging is, as the head unit does beside its own settings: the plan
    // holds to that figure, and names it. The package's own files do not fit either now, and the plan says both.
    f.platform.reserve = 1u << 20;
    {
        std::string text = f.call(q, "job.plan", planParams(kRuntime, "", kRuntime));
        Parsed tight(text);
        json::Value problems = tight.result.find("problems");
        CHECK_IN(text, !tight.result.find("planId").present() && problems.size() == 2);
        for (size_t i = 0; i < problems.size(); ++i) {
            CHECK_IN(text, str(problems.at(i), "code") == "no-space" && has(str(problems.at(i), "message"), "1048576 are kept free"));
        }
        CHECK_IN(text, has(text, "on the file system that holds staging"));
    }
    f.platform.fakeFree = (1 << 20) + 2000;
    planOk(f, q, kRuntime, "", kRuntime);
    f.platform.reserve = 64u << 10;
    f.platform.fakeFree = -1;

    // One plan at a time; a new one takes the place of the last.
    std::string first = planOk(f, q, kRuntime, "", kRuntime);
    std::string second = planOk(f, q, kRuntime, "", kRuntime);
    CHECK(first != second && Parsed(startJob(f, q, first)).code() == "not-found");
    Phone* slow = f.phone("2");
    slow->serve = false;
    int64_t held = slow->request("job.plan", planParams(kRuntime, "", kRuntime));
    f.settle();
    CHECK(slow->replies.count(held) == 0 && slow->heldFetches.size() == 1);
    CHECK(Parsed(f.call(q, "job.plan", planParams(kRuntime, "", kRuntime))).code() == "busy");
    // The older plan went when the newer one began, even though that one has not finished.
    CHECK(Parsed(startJob(f, q, second)).code() == "not-found");
    // The phone that was planning goes away: the plan is dropped, and planning is free again.
    slow->hangUp();
    f.settle();
    planOk(f, q, kRuntime, "", kRuntime);
}

// ---- a job from start to end --------------------------------------------------------------------------------------

static void testJob() {
    Bench b("job");
    b.agent.job().setFetchChunk(700);   // every archive takes several blob.fetch requests,
    Phone* p = b.phone("1", 1024);
    p->dataFrame = 300;                 // and every range several DATA frames
    Phone* watcher = b.phone("2");
    std::string planId = planOk(b, p, kHello, "", kHelloAndRuntime);
    p->fetches.clear();
    p->streamed = 0;   // the plan pulled two manifests; from here on only archives count

    std::string started = startJob(b, p, planId);
    Parsed s(started);
    json::Value job = s.result.find("job");
    // The reply describes the job as it began; by now the test has let it run to its end.
    CHECK_IN(started, str(job, "planId") == planId && str(job, "state") == "fetching" && integer(job, "step") == 1 &&
                          integer(job, "steps") == 2 && str(job, "id") == "org.nbtapp.runtime" && integer(job, "seq") == 0);
    CHECK_IN(outcome(p), outcome(p) == "succeeded done done");
    CHECK(b.versionOf("org.nbtapp.hello") == "1.2.0" && b.versionOf("org.nbtapp.runtime") == "2.1.0");
    CHECK(fs::kindOf(b.app("org.nbtapp.hello") + "/bin/hello") == fs::kFile);

    // The archives came in ranges of at most 700 bytes, each from where the last one ended.
    size_t ranges = 0;
    uint64_t next = 0;
    for (size_t i = 0; i < p->fetches.size(); ++i) {
        std::vector<std::string> f = words(p->fetches[i]);
        if (f[0] != pkg(kRuntime).sha256) continue;
        CHECK_IN(p->fetches[i], f[1] == num((long long)next) && atoll(f[2].c_str()) <= 700 && atoll(f[2].c_str()) >= 1);
        next += (uint64_t)atoll(f[2].c_str());
        ++ranges;
    }
    CHECK(next == pkg(kRuntime).size && ranges == (pkg(kRuntime).size + 699) / 700);
    CHECK(p->streamed == pkg(kRuntime).size + pkg(kHello).size);
    // Once the job has succeeded the archives are gone from staging.
    std::vector<std::string> left;
    CHECK(fs::listDir(b.agent.engine().store().blobDir(), &left, 0) && left.empty());

    // The events: numbered from 1 with no gap, the same for every controller, in the order of the work.
    CHECK(p->events.size() == watcher->events.size() && p->events.size() > 10);
    std::string phases;
    std::string hookText;
    for (size_t i = 0; i < p->events.size(); ++i) {
        CHECK(p->events[i] == watcher->events[i]);
        Parsed e(p->events[i]);
        CHECK_IN(p->events[i], integer(e.root, "seq") == (long long)i + 1 && str(e.params, "planId") == planId);
        std::string name = str(e.root, "event");
        if (name == "job.progress") {
            std::string at = str(e.params, "state") + ":" + num(integer(e.params, "step")) + ":" + str(e.params, "phase");
            if (!has(phases, at + " ")) phases += at + " ";
            CHECK(integer(e.params, "steps") == 2);
            if (str(e.params, "phase") == "fetch") CHECK(integer(e.params, "bytes") >= 0 && integer(e.params, "total") > 0);
        } else if (name == "job.log") {
            if (str(e.params, "source") == "postInstall") hookText += str(e.params, "text");
            CHECK(integer(e.params, "step") >= 1 && integer(e.params, "step") <= 2);
        } else {
            CHECK(name == "job.done" && i + 1 == p->events.size());
        }
    }
    CHECK_IN(phases, phases == "fetching:1:fetch fetching:2:fetch installing:1:<none> installing:1:verify installing:1:pre-install "
                               "installing:1:backup installing:1:files installing:1:post-install installing:1:record "
                               "installing:2:<none> installing:2:verify installing:2:pre-install installing:2:backup "
                               "installing:2:files installing:2:post-install installing:2:record ");
    CHECK_IN(hookText, has(hookText, "post-install: app dir " + b.app("org.nbtapp.hello")));
    Parsed done(lastEvent(p, "job.done"));
    CHECK(str(done.params, "state") == "succeeded" && str(done.params, "restart") == "none" && done.params.find("steps").size() == 2);
    CHECK(str(done.params.find("steps").at(1), "action") == "install" && str(done.params.find("steps").at(1), "version") == "1.2.0");

    // job.status: the job with its result; and the kept events again, here over a link with small frames, so in
    // several pages. They are the job.log and job.done events exactly as they were sent.
    Parsed status(b.call(watcher, "job.status", "{}"));
    json::Value sj = status.result.find("job");
    CHECK(str(sj, "state") == "succeeded" && integer(sj, "seq") == (long long)p->events.size() && status.result.find("events").size() == 0);
    CHECK(str(sj.find("result"), "state") == "succeeded" && sj.find("result").find("steps").size() == 2);
    std::vector<std::string> kept, replayed;
    for (size_t i = 0; i < p->events.size(); ++i) {
        if (!has(p->events[i], "\"event\":\"job.progress\"")) kept.push_back(p->events[i]);
    }
    long long after = 0;
    int pages = 0;
    for (;;) {
        std::string text = b.call(p, "job.status", "{\"planId\":\"" + planId + "\",\"after\":" + num(after) + "}");
        CHECK(text.size() <= 1024);
        Parsed page(text);
        json::Value events = page.result.find("events");
        for (size_t i = 0; i < events.size(); ++i) {
            after = integer(events.at(i), "seq");
            // Compared as values: the text of each is checked through its seq and its place in `kept`.
            replayed.push_back(num(after));
        }
        ++pages;
        if (!page.result.find("more").boolean()) break;
        CHECK(events.size() > 0);
    }
    CHECK(pages > 1 && replayed.size() == kept.size());
    for (size_t i = 0; i < kept.size(); ++i) CHECK(num(integer(Parsed(kept[i]).root, "seq")) == replayed[i]);
    Parsed tail(b.call(watcher, "job.status", "{\"after\":" + num((long long)p->events.size() - 1) + "}"));
    CHECK(tail.result.find("events").size() == 1 && str(tail.result.find("events").at(0), "event") == "job.done");
    CHECK(tail.result.find("events").at(0).find("params").find("steps").size() == 2);

    // job.start for the job that has ended changes nothing and returns it; its plan cannot be run again.
    size_t before = p->events.size();
    Parsed again(startJob(b, p, planId));
    CHECK(str(again.result.find("job"), "state") == "succeeded" && p->events.size() == before);
    CHECK(Parsed(b.call(p, "job.cancel", "{\"planId\":\"" + planId + "\"}")).code() == "too-late");
    CHECK(Parsed(startJob(b, p, "ffffffffffffffff")).code() == "not-found");
    CHECK(Parsed(startJob(b, p, "nope")).code() == "bad-request");

    // An upgrade, then a removal of both. A job with nothing to fetch starts as installing.
    Bench u("job-upgrade");
    Phone* q = u.phone();
    install(u, q, kHelloOld, std::string(kHelloOld) + " " + kRuntime);
    q->events.clear();
    std::string upgrade = u.call(q, "job.plan", planParams(kHello, "", kHello));
    Parsed up(upgrade);
    CHECK_IN(upgrade, up.result.find("steps").size() == 1 && str(up.result.find("steps").at(0), "action") == "upgrade" &&
                          str(up.result.find("steps").at(0), "oldVersion") == "1.1.0");
    Parsed upStart(startJob(u, q, str(up.result, "planId")));
    CHECK(outcome(q) == "succeeded done" && u.versionOf("org.nbtapp.hello") == "1.2.0");
    CHECK(str(Parsed(lastEvent(q, "job.done")).params.find("steps").at(0), "action") == "upgrade");

    q->events.clear();
    q->fetches.clear();
    std::string removal = planOk(u, q, "", "org.nbtapp.runtime org.nbtapp.hello!", "");
    Parsed rm(startJob(u, q, removal));
    CHECK(str(rm.result.find("job"), "state") == "installing" && q->fetches.empty());
    CHECK(outcome(q) == "succeeded done done" && !u.installed("org.nbtapp.hello") && !u.installed("org.nbtapp.runtime"));
    CHECK(fs::kindOf(u.platform.dataDir("org.nbtapp.hello")) == fs::kNone && fs::kindOf(u.platform.dataDir("org.nbtapp.runtime")) == fs::kDir);
    std::string removePhases;
    for (size_t i = 0; i < q->events.size(); ++i) {
        Parsed e(q->events[i]);
        if (str(e.root, "event") == "job.progress" && integer(e.params, "step") == 1 && str(e.params, "phase") != "<none>") removePhases += str(e.params, "phase") + " ";
    }
    CHECK_IN(removePhases, removePhases == "pre-remove remove post-remove record ");
}

// A step that fails ends the job: it is undone, steps before it stay, steps after it are skipped.
static void testJobFails() {
    Bench b("job-fails");
    Phone* p = b.phone();
    install(b, p, kHelloOld, std::string(kHelloOld) + " " + kRuntime);
    p->events.clear();
    // runtime 2.1.0 is installed; the plugin goes in, the hello upgrade fails in postInstall, and rival2 is not tried.
    std::string targets = "org.nbtapp.demoplugin-1.0.0 org.nbtapp.hello-1.3.0 org.nbtapp.rival2-1.0.0";
    std::string planId = planOk(b, p, targets, "", targets);
    startJob(b, p, planId);
    CHECK_IN(outcome(p), outcome(p) == "failed done failed(hook-failed) skipped");
    Parsed done(lastEvent(p, "job.done"));
    CHECK(str(done.params, "restart") == "hmi-restart");   // what the plugin, which is installed, needs
    CHECK(has(str(done.params.find("steps").at(1).find("error"), "message"), "postInstall exited with 3"));
    CHECK(b.versionOf("org.nbtapp.hello") == "1.1.0" && b.installed("org.nbtapp.demoplugin") && !b.installed("org.nbtapp.rival2"));
    Parsed status(b.call(p, "job.status", "{}"));
    CHECK(str(status.result.find("job"), "state") == "failed");
    // A failed job keeps its archives: the next try fetches nothing.
    Parsed retry(b.call(p, "job.plan", planParams("org.nbtapp.rival2-1.0.0", "", "org.nbtapp.rival2-1.0.0")));
    CHECK(integer(retry.result, "fetchBytes") == 0);
    p->fetches.clear();
    p->events.clear();
    startJob(b, p, str(retry.result, "planId"));
    CHECK(outcome(p) == "succeeded done" && b.installed("org.nbtapp.rival2"));
    for (size_t i = 0; i < p->fetches.size(); ++i) CHECK(words(p->fetches[i])[0] != pkg("org.nbtapp.rival2-1.0.0").sha256);

    // A hook that is killed at its timeout.
    p->events.clear();
    startJob(b, p, planOk(b, p, "org.nbtapp.slow-1.0.0", "", "org.nbtapp.slow-1.0.0"));
    CHECK_IN(outcome(p), outcome(p) == "failed failed(hook-timeout)");

    // The head unit changed between the plan and the start.
    std::string stale = planOk(b, p, "", "org.nbtapp.rival2", "");
    StepReport direct = b.agent.engine().remove("org.nbtapp.demoplugin", false, 0);
    CHECK(direct.outcome == kStepDone);
    CHECK(Parsed(startJob(b, p, stale)).code() == "plan-stale");
    CHECK(Parsed(startJob(b, p, stale)).code() == "not-found");   // a stale plan is gone
    CHECK(b.installed("org.nbtapp.rival2"));

    // What only the step can see: the plugin's HMI ids are inside the archive. A plugin that is on the car
    // already, from outside NBTApp, has the connector id the package wants.
    std::string why;
    std::string foreign = b.platform.pluginsDir() + "/Other";
    CHECK(fs::makeDirs(foreign, &why));
    CHECK(fs::writeFile(foreign + "/Other.manifest",
                        "<plugin><name>Other</name><config><dependencies><pluginAppConnector><id>250</id>"
                        "</pluginAppConnector></dependencies></config></plugin>\n", 0644, &why));
    p->events.clear();
    startJob(b, p, planOk(b, p, "org.nbtapp.demoplugin-1.0.0", "", "org.nbtapp.demoplugin-1.0.0"));
    CHECK_IN(outcome(p), outcome(p) == "failed failed(hmi-id-taken)");
}

// A hook's output reaches the controllers whole, as text: in pieces of at most 4,096 bytes that never end inside a
// character, with whatever is not UTF-8 replaced.
static void testHookOutput() {
    CHECK(sanitizeUtf8("plain", 0) == "plain" && sanitizeUtf8("", 0) == "");
    CHECK(sanitizeUtf8("a\xFFz", 0) == "a\xEF\xBF\xBDz");
    CHECK(sanitizeUtf8("\xC3\xA4\xE2\x9C\x93\xF0\x9F\x99\x82", 0) == "\xC3\xA4\xE2\x9C\x93\xF0\x9F\x99\x82");
    CHECK(sanitizeUtf8("\xC0\xAF", 0) == "\xEF\xBF\xBD\xEF\xBF\xBD");                 // an overlong form
    CHECK(sanitizeUtf8("\xED\xA0\x80", 0) == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");   // a surrogate
    CHECK(sanitizeUtf8("x\xE2\x9C", 0) == "x\xEF\xBF\xBD\xEF\xBF\xBD");               // cut off, and no more is coming
    std::string tail;
    CHECK(sanitizeUtf8("x\xE2\x9C", &tail) == "x" && tail == "\xE2\x9C");               // cut off, and more may come
    CHECK(sanitizeUtf8(tail + "\x93y", &tail) == "\xE2\x9C\x93y" && tail.empty());
    CHECK(sanitizeUtf8("x\xE2\x41", &tail) == "x\xEF\xBF\xBD\x41" && tail.empty());      // wrong, not cut off

    Bench b("hook-output");
    Phone* p = b.phone();
    install(b, p, "org.nbtapp.noisy-1.0.0", "org.nbtapp.noisy-1.0.0");
    std::string said, expected;
    size_t pieces = 0;
    for (size_t i = 0; i < p->events.size(); ++i) {
        Parsed e(p->events[i]);   // which also says that the text is UTF-8: the parser refuses anything else
        if (str(e.root, "event") != "job.log" || str(e.params, "source") != "postInstall") continue;
        std::string text = str(e.params, "text");
        CHECK(text.size() <= 4096 && !text.empty());
        said += text;
        ++pieces;
    }
    for (int i = 0; i < 400; ++i) {
        char line[64];
        snprintf(line, sizeof line, "line %03d \xC3\xA4\xC3\xB6\xC3\xBC \xE2\x9C\x93 some more text\n", i);
        expected += line;
    }
    expected += "a byte that is not text: \xEF\xBF\xBD\n";
    CHECK(said == expected && pieces >= 4);
    // And job.status hands the same text out again.
    Parsed status(b.call(p, "job.status", "{\"after\":0}"));
    json::Value events = status.result.find("events");
    std::string replayed;
    for (size_t i = 0; i < events.size(); ++i) {
        if (str(events.at(i).find("params"), "source") == "postInstall") replayed += str(events.at(i).find("params"), "text");
    }
    CHECK(replayed == expected && !status.result.find("more").boolean());
}

// ---- the agent's log ----------------------------------------------------------------------------------------------

static void testLog() {
    Bench b("log");
    std::string path = b.root + "/log.txt";
    FILE* log = fopen(path.c_str(), "w");
    CHECK(log != 0);
    b.agent.setLog(log);

    // A phone whose name tries to write a line of its own into the log.
    Phone* p = b.connect();
    Parsed hello(b.call(p, "hello", "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":\"" + std::string(32, '7') +
                                    "\",\"name\":\"Mallory\\n00:00:00.000 nbtagent: forged\",\"maxFrame\":65536,\"capabilities\":[]}"));
    CHECK_IN(hello.code(), hello.result.isObject());
    for (std::map<std::string, Pkg>::const_iterator it = gPkgs.begin(); it != gPkgs.end(); ++it) {
        p->blobs[it->second.sha256] = it->second.bytes;
        p->blobs[it->second.manifestSha256] = it->second.manifest;
    }
    CHECK(Parsed(b.call(p, "job.start", "{}")).code() == "bad-request");
    install(b, p, "org.nbtapp.hello-1.1.0", "org.nbtapp.hello-1.1.0 org.nbtapp.runtime-2.1.0");
    b.agent.setLog(0);
    fclose(log);

    std::string text = testutil::readFile(path);
    // Every line is the agent's own: a time of day, then its name, and nothing in it that is not print.
    size_t lines = 0;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        CHECK(end != std::string::npos);
        std::string line = text.substr(at, end - at);
        CHECK_IN(line, line.size() > 23 && line[2] == ':' && line[5] == ':' && line[8] == '.' && line.compare(12, 11, " nbtagent: ") == 0);
        for (size_t i = 0; i < line.size(); ++i) CHECK_IN(line, (unsigned char)line[i] >= 0x20 || line[i] == '\t');
        CHECK_IN(line, line.find("forged") == std::string::npos || line.find("Mallory?00:00:00.000 nbtagent: forged") != std::string::npos);
        ++lines;
        at = end + 1;
    }
    // What happened, in the order it happened.
    const char* expected[] = {
        "link 1 opened (local)",
        "link 1: hello from a phone called \"Mallory?",
        "link 1: hello -> ok",
        "link 1: job.start -> bad-request: job.start takes a planId of 16 hex characters",
        "link 1: job.plan asks to install org.nbtapp.hello 1.1.0; 2 packages on offer",
        ", a manifest",
        ": 2 steps, ",
        "  install org.nbtapp.runtime 2.1.0",
        "link 1: job.plan -> ok",
        " started, over link 1",
        "job fetching, step 1 of 2 (org.nbtapp.runtime): fetch",
        " bytes from 0",
        "job installing, step 2 of 2 (org.nbtapp.hello): post-install",
        "job, step 2: postInstall| post-install: app dir ",
        " succeeded, restart needed: none",
        "  install org.nbtapp.hello 1.1.0: done",
    };
    size_t from = 0;
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
        size_t found = text.find(expected[i], from);
        CHECK_IN(std::string(expected[i]) + "\n" + text, found != std::string::npos);
        from = found;
    }
    CHECK(lines > 30);
}

// ---- fetching: pauses, cancels, and what a phone can do wrong -----------------------------------------------------

static void testPauseAndResume() {
    Bench b("pause");
    b.agent.job().setFetchChunk(400);
    Phone* p = b.phone("1");
    Phone* watcher = b.phone("2");
    std::string planId = planOk(b, p, kHello, "", kHelloAndRuntime);
    p->stallAt = 150;   // the phone goes quiet in the middle of the first range of the first archive ...
    p->fetches.clear();
    p->streamed = 0;
    startJob(b, p, planId);
    CHECK(str(Parsed(b.call(watcher, "job.status", "{}")).result.find("job"), "state") == "fetching");
    uint64_t sentBefore = p->streamed;
    CHECK(sentBefore == 150);
    p->hangUp();   // and then is gone
    b.settle();
    Parsed paused(b.call(watcher, "job.status", "{}"));
    CHECK(str(paused.result.find("job"), "state") == "paused" && integer(paused.result.find("job"), "bytes") == 150);
    CHECK(has(lastEvent(watcher, "job.progress"), "\"state\":\"paused\""));
    // A paused job is not in the way of a plan, and waits for as long as it takes.
    b.clock.now += 3600 * 1000;
    b.settle();
    CHECK(b.agent.job().paused());

    // The phone is back on a new link and starts the job again. The fetch goes on from the byte it stopped at.
    Phone* back = b.phone("1");
    Parsed resumed(startJob(b, back, planId));
    CHECK(str(resumed.result.find("job"), "planId") == planId);
    CHECK_IN(outcome(back), outcome(back) == "succeeded done done");
    CHECK(back->fetches.size() >= 1 && back->fetches[0] == pkg(kRuntime).sha256 + " 150 400");
    CHECK(back->streamed == pkg(kRuntime).size + pkg(kHello).size - 150);
    CHECK(b.versionOf("org.nbtapp.hello") == "1.2.0");

    // A paused job gives way to another job; what it had fetched of an archive both name is kept.
    Bench r("pause-replace");
    Phone* a = r.phone("1");
    std::string firstPlan = planOk(r, a, kRuntime, "", kRuntime);
    a->stallAt = 100;
    startJob(r, a, firstPlan);
    a->hangUp();
    r.settle();
    CHECK(r.agent.job().paused());
    Phone* c = r.phone("3");
    std::string text = r.call(c, "job.plan", planParams(kHello, "", kHelloAndRuntime));
    Parsed plan(text);
    CHECK_IN(text, integer(plan.result, "fetchBytes") == (long long)(pkg(kHello).size + pkg(kRuntime).size - 100));
    std::string secondPlan = str(plan.result, "planId");
    // With the first job started again, the second plan has to wait: one job at a time.
    Phone* a2 = r.phone("1");
    a2->stallAt = 50;
    startJob(r, a2, firstPlan);
    CHECK(r.agent.job().fetching() && Parsed(startJob(r, c, secondPlan)).code() == "busy");
    a2->hangUp();
    r.settle();
    startJob(r, c, secondPlan);
    CHECK_IN(outcome(c), outcome(c) == "succeeded done done");
    CHECK(c->fetches.size() >= 1);
    bool resumedAt150 = false;
    for (size_t i = 0; i < c->fetches.size(); ++i) resumedAt150 = resumedAt150 || has(c->fetches[i], pkg(kRuntime).sha256 + " 150 ");
    CHECK(resumedAt150);
    CHECK(Parsed(r.call(c, "job.status", "{\"planId\":\"" + firstPlan + "\"}")).code() == "not-found");
}

static void testCancel() {
    Bench b("cancel");
    Phone* p = b.phone();
    std::string cancel;

    // job.cancel while a stream is open: the job ends as cancelled, the stream runs out unused, nothing changed.
    std::string planId = planOk(b, p, kHello, "", kHelloAndRuntime);
    cancel = "{\"planId\":\"" + planId + "\"}";
    p->stallAt = 200;
    startJob(b, p, planId);
    CHECK(b.agent.job().fetching());
    CHECK(Parsed(b.call(p, "job.cancel", cancel)).result.isObject());
    CHECK_IN(outcome(p), outcome(p) == "cancelled skipped skipped");
    CHECK(Parsed(b.call(p, "job.cancel", cancel)).result.isObject());   // twice is harmless
    CHECK(Parsed(b.call(p, "job.cancel", "{\"planId\":\"0000000000000000\"}")).code() == "not-found");
    // One stream at a time: while the old one is still open, a plan that needs a manifest waits for it to end.
    int64_t waiting = p->request("job.plan", planParams(kRuntime, "", kRuntime));
    b.settle();
    CHECK(p->replies.count(waiting) == 0);
    p->stallAt = -1;
    b.settle();   // the rest of the stream arrives and is dropped
    CHECK(Parsed(p->reply(waiting)).result.find("planId").isString());
    CHECK(!p->closed && !b.installed("org.nbtapp.runtime"));
    // The next job gets the link although the old stream had been open, and fetches only what is missing.
    p->events.clear();
    install(b, p, kRuntime, kRuntime);

    // The phone's own way of stopping: blob.error with `cancelled`, then job.cancel.
    planId = planOk(b, p, kHello, "", kHello);
    p->errorAt = 300;
    p->errorCode = "cancelled";
    p->events.clear();
    startJob(b, p, planId);
    CHECK_IN(outcome(p), outcome(p) == "cancelled skipped");
    CHECK(Parsed(b.call(p, "job.cancel", "{\"planId\":\"" + planId + "\"}")).result.isObject());

    // A stream that breaks is asked for again from where it broke, and the job goes on.
    planId = planOk(b, p, kHello, "", kHello);
    p->errorAt = 100;
    p->errorCode = "io-error";
    p->events.clear();
    p->fetches.clear();
    startJob(b, p, planId);
    CHECK_IN(outcome(p), outcome(p) == "succeeded done");
    CHECK(p->fetches.size() == 2 && words(p->fetches[1])[1] == "400");   // 300 were there from the job before

    // The phone no longer has the archive; the phone keeps failing; the archive is not the one that was offered.
    Bench f("cancel-fetch-fails");
    Phone* q = f.phone();
    for (int round = 0; round < 3; ++round) {
        std::string id = planOk(f, q, kRuntime, "", kRuntime);
        q->events.clear();
        std::string good = q->blobs[pkg(kRuntime).sha256];
        if (round == 0) {
            q->errorAt = 10;
            q->errorCode = "not-found";
        } else if (round == 1) {
            q->serve = false;
        } else {
            q->blobs[pkg(kRuntime).sha256][good.size() - 1] ^= 1;
        }
        startJob(f, q, id);
        for (int tries = 0; round == 1 && tries < 3; ++tries) {
            q->failNext = "io-error";
            q->answerHeld();
            f.settle();
        }
        q->serve = true;
        CHECK_IN(num(round) + ": " + outcome(q), outcome(q) == "failed failed(fetch-failed)");
        q->blobs[pkg(kRuntime).sha256] = good;
        CHECK(!f.installed("org.nbtapp.runtime") && !q->closed);
    }
    // After a wrong hash nothing of the archive is kept: the next job fetches all of it.
    Parsed fresh(f.call(q, "job.plan", planParams(kRuntime, "", kRuntime)));
    CHECK(integer(fresh.result, "fetchBytes") == (long long)pkg(kRuntime).size);
}

static void testFetchFaults() {
    // A phone that never answers, or stops in the middle of a stream, loses its link after the timeout, and the
    // job waits as paused.
    for (int round = 0; round < 2; ++round) {
        Bench b(round == 0 ? "fault-silent" : "fault-stall");
        b.agent.setFetchTimeoutMs(5000);
        Phone* p = b.phone();
        std::string planId = planOk(b, p, kRuntime, "", kRuntime);
        if (round == 0) p->serve = false;
        else p->stallAt = 64;
        startJob(b, p, planId);
        b.clock.now += 4000;
        b.settle();
        CHECK(!p->closed && b.agent.job().fetching());
        b.clock.now += 2000;
        b.settle();
        CHECK(p->closed && b.agent.job().paused());
    }
    // What breaks the rules ends the link: a reply for another length, a stream longer than its reply said, and a
    // stream that ends early.
    const char* cases[] = {"size", "long", "short"};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        std::string what = cases[i];
        Bench b("fault-" + what);
        Phone* p = b.phone();
        std::string planId = planOk(b, p, kRuntime, "", kRuntime);
        if (what == "size") {
            p->lieAboutSize = 1;
            startJob(b, p, planId);
        } else {
            p->serve = false;
            startJob(b, p, planId);
            CHECK(p->heldFetches.size() == 1);
            int64_t id = Parsed(p->heldFetches[0]).root.find("id").integer();
            size_t size = pkg(kRuntime).size;
            p->sendMessage("{\"id\":" + num(id) + ",\"result\":{\"stream\":4,\"size\":" + num((long long)size) + "}}");
            if (what == "long") {
                p->sendFrame(2, 0, 4, pkg(kRuntime).bytes);
                p->sendFrame(2, 2, 4, "x");
            } else {
                p->sendFrame(2, 2, 4, pkg(kRuntime).bytes.substr(0, size - 1));
            }
            b.settle();
        }
        CHECK_IN(what, p->closed && b.agent.job().paused() && !b.installed("org.nbtapp.runtime"));
    }
}

// ---- while a step runs --------------------------------------------------------------------------------------------

// Writes to the agent's links from inside an install step: the engine calls this at the points where a crash test
// would end the process, which is in the middle of the step.
class MidStep : public CrashHook {
public:
    MidStep() : phone(0), newcomer(0), done(false) {}
    virtual void point(const char*) {
        if (done) return;
        done = true;
        ids.push_back(phone->request("job.status", "{}"));
        ids.push_back(phone->request("packages.list", "{}"));
        ids.push_back(phone->request("job.plan", planParams(kRuntime, "", kRuntime)));
        ids.push_back(phone->request("job.cancel", "{\"planId\":\"" + planId + "\"}"));
        ids.push_back(phone->request("job.start", "{\"planId\":\"" + planId + "\"}"));
        ids.push_back(phone->request("system.restart", "{\"kind\":\"hmi\"}"));
        ids.push_back(phone->request("info.get", "{}"));
        phone->pump();
        hello = newcomer->request("hello", kGoodHello);
        status = newcomer->request("job.status", "{}");
        newcomer->pump();
    }
    Phone* phone;
    Phone* newcomer;
    std::string planId;
    std::vector<int64_t> ids;
    int64_t hello, status;
    bool done;
};

static void testDuringStep() {
    Bench b("during-step");
    Phone* p = b.phone();
    install(b, p, kRuntime, kRuntime);
    MidStep mid;
    mid.phone = p;
    mid.newcomer = b.connect();   // connected, and silent until the step is under way
    mid.planId = planOk(b, p, kHello, "", kHello);
    b.agent.engine().setCrashHook(&mid);
    p->events.clear();
    startJob(b, p, mid.planId);
    b.agent.engine().setCrashHook(0);
    CHECK(mid.done && outcome(p) == "succeeded done");

    // Everything was answered while the step ran: the replies come before job.done.
    Parsed status(p->reply(mid.ids[0]));
    CHECK(str(status.result.find("job"), "state") == "installing" && str(status.result.find("job"), "id") == "org.nbtapp.hello");
    CHECK(Parsed(p->reply(mid.ids[1])).code() == "busy");
    CHECK(Parsed(p->reply(mid.ids[2])).code() == "busy");
    CHECK(Parsed(p->reply(mid.ids[3])).code() == "too-late");
    CHECK(str(Parsed(p->reply(mid.ids[4])).result.find("job"), "state") == "installing");
    CHECK(Parsed(p->reply(mid.ids[5])).code() == "busy");
    CHECK(str(Parsed(p->reply(mid.ids[6])).result, "headUnitId") == kHeadUnitId);
    // A controller that connected during the step was greeted, saw the job, and got its remaining events.
    CHECK(Parsed(mid.newcomer->reply(mid.hello)).result.isObject());
    CHECK(str(Parsed(mid.newcomer->reply(mid.status)).result.find("job"), "state") == "installing");
    CHECK(has(lastEvent(mid.newcomer, "job.done"), "succeeded"));
    CHECK(b.platform.hmiRestarts == 0);
}

// ---- restarts -----------------------------------------------------------------------------------------------------

static void testSystemRestart() {
    Bench b("restart");
    Phone* p = b.phone();
    CHECK(Parsed(b.call(p, "system.restart", "{\"kind\":\"warm\"}")).code() == "bad-request");
    CHECK(Parsed(b.call(p, "system.restart", "{}")).code() == "bad-request");
    CHECK(b.platform.hmiRestarts == 0 && b.platform.reboots == 0);

    install(b, p, "org.nbtapp.demoplugin-1.0.0", "org.nbtapp.demoplugin-1.0.0");
    CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "hmi-restart");
    // The answer comes first, the restart after it.
    int64_t id = p->request("system.restart", "{\"kind\":\"hmi\"}");
    b.settle();
    CHECK(Parsed(p->reply(id)).result.isObject() && b.platform.hmiRestarts == 1 && b.platform.reboots == 0);
    CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "none");

    // A package that needs a reboot is not satisfied by an HMI restart.
    std::string why;
    CHECK(fs::makeDirs(b.platform.systemPath("/opt/demo"), &why));
    CHECK(fs::writeFile(b.platform.systemPath("/opt/demo/menu.xml"), "stock menu, firmware A\n", 0644, &why));
    install(b, p, "org.nbtapp.menupatch-1.0.0", "org.nbtapp.menupatch-1.0.0");
    CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "required");
    CHECK(Parsed(b.call(p, "system.restart", "{\"kind\":\"hmi\"}")).result.isObject());
    CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "required" && b.platform.hmiRestarts == 2);
    CHECK(Parsed(b.call(p, "system.restart", "{\"kind\":\"reboot\"}")).result.isObject());
    CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "none" && b.platform.reboots == 1);

    // A head unit with no way to do it says so, and does not answer yes first.
    b.platform.noReboot = "no way to reboot is known for this head unit";
    Parsed no(b.call(p, "system.restart", "{\"kind\":\"reboot\"}"));
    CHECK(no.code() == "unsupported" && str(no.error, "message") == b.platform.noReboot && b.platform.reboots == 1);
    CHECK(Parsed(b.call(p, "system.restart", "{\"kind\":\"hmi\"}")).result.isObject() && b.platform.hmiRestarts == 3);
    b.platform.noReboot.clear();
    b.platform.noHmiRestart = "this head unit has no /opt/sys/bin/restarter to restart the HMI with";
    CHECK(Parsed(b.call(p, "system.restart", "{\"kind\":\"hmi\"}")).code() == "unsupported" && b.platform.hmiRestarts == 3);
    b.platform.noHmiRestart.clear();

    // Not while a job is fetching.
    std::string planId = planOk(b, p, kRuntime, "", kRuntime);
    p->serve = false;
    startJob(b, p, planId);
    CHECK(Parsed(b.call(p, "system.restart", "{\"kind\":\"reboot\"}")).code() == "busy" && b.platform.reboots == 1);
}

// The agent stops and starts again: what it knew about the last job is still there.
static void testAgentRestart() {
    std::string planId;
    size_t eventCount = 0;
    {
        Bench b("agent-restart");
        Phone* p = b.phone();
        install(b, p, kHello, kHelloAndRuntime);
        planId = b.agent.job().planId();
        eventCount = p->events.size();
    }
    {
        Bench b("agent-restart", true);
        Phone* p = b.phone();
        Parsed status(b.call(p, "job.status", "{\"after\":0}"));
        json::Value job = status.result.find("job");
        CHECK(str(job, "planId") == planId && str(job, "state") == "succeeded" && integer(job, "seq") == (long long)eventCount);
        json::Value events = status.result.find("events");
        CHECK(events.size() > 3 && str(events.at(events.size() - 1), "event") == "job.done");
        CHECK(b.versionOf("org.nbtapp.hello") == "1.2.0");
        // A plan does not outlast the agent; the job's own id still answers.
        CHECK(str(Parsed(startJob(b, p, planId)).result.find("job"), "state") == "succeeded");
    }

    // A job that was fetching when the agent stopped: it is over, and nothing was changed.
    {
        Bench b("agent-restart-fetch");
        Phone* p = b.phone();
        planId = planOk(b, p, kHello, "", kHelloAndRuntime);
        p->stallAt = 100;
        startJob(b, p, planId);
        CHECK(b.agent.job().fetching());
    }
    {
        Bench b("agent-restart-fetch", true);
        Phone* p = b.phone();
        Parsed status(b.call(p, "job.status", "{\"after\":0}"));
        json::Value job = status.result.find("job"), steps = job.find("result").find("steps");
        CHECK(str(job, "state") == "failed" && steps.size() == 2);
        CHECK(str(steps.at(0), "result") == "failed" && str(steps.at(0).find("error"), "code") == "interrupted" && str(steps.at(1), "result") == "skipped");
        // The numbers went on from far above, so that no number a controller saw before is used twice.
        CHECK(integer(job, "seq") > 1000000);
        json::Value events = status.result.find("events");
        CHECK(events.size() >= 2 && str(events.at(events.size() - 1), "event") == "job.done");
        CHECK(!b.installed("org.nbtapp.runtime"));
    }
}

// A power cut takes the run folder with it. Staging may come through one with its files, and the next agent
// empties it: a fetch never goes on from before a power down. An agent that is only started again keeps them.
static size_t countIn(const std::string& dir) {
    std::vector<std::string> names;
    CHECK(fs::listDir(dir, &names, 0));
    return names.size();
}

static void testPowerCut() {
    std::string blobs, run;
    {
        Bench b("power-cut");
        Phone* p = b.phone();
        // The marks are in the run folder, none of them in staging.
        blobs = b.agent.engine().store().blobDir();
        run = b.platform.runDir();
        install(b, p, "org.nbtapp.demoplugin-1.0.0", "org.nbtapp.demoplugin-1.0.0");
        CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "hmi-restart");
        CHECK(fs::kindOf(run + "/agent-started") == fs::kFile && fs::kindOf(run + "/restart-needed") == fs::kFile);
        CHECK(countIn(b.platform.stagingDir()) == 2 && countIn(blobs) == 0);   // blobs and unpack, and nothing else
        // A fetch that the agent's end cuts off.
        std::string planId = planOk(b, p, kHello, "", kHelloAndRuntime);
        p->stallAt = 100;
        startJob(b, p, planId);
        CHECK(b.agent.job().fetching() && countIn(blobs) == 1);
    }
    {
        // Started again on a head unit that stayed up: what was fetched is still there, and so is the need to restart.
        Bench b("power-cut", true);
        Phone* p = b.phone();
        CHECK(countIn(blobs) == 1);
        CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "hmi-restart");
        CHECK(b.agent.job().state() == "failed");
    }
    std::string why;
    CHECK_IN(why, fs::removeTree(run, &why));   // the power cut
    {
        Bench b("power-cut", true);
        Phone* p = b.phone();
        CHECK(countIn(blobs) == 0 && fs::isDir(b.agent.engine().store().unpackDir()));
        CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "none");
        CHECK(fs::kindOf(run + "/agent-started") == fs::kFile && b.installed("org.nbtapp.demoplugin"));
        // And the next job fetches from the start.
        install(b, p, kHello, kHelloAndRuntime);
        CHECK(b.versionOf("org.nbtapp.hello") == "1.2.0" && countIn(blobs) == 0);
    }
}

// An agent that is started on a head unit where nothing waits to be finished writes nothing to flash: the state
// and staging are read, and left as they are to the last time stamp. Only the run folder, which is on the RAM
// disk, is written to. What is held against what: every name with its kind, mode, size, inode and the times of
// its last change, which a write of any kind moves. The time of last access is not looked at.
static void addToSnapshot(const std::string& path, std::string* out) {
    struct stat st;
    CHECK_IN(path, lstat(path.c_str(), &st) == 0);
    long mtimeNs = 0, ctimeNs = 0;
#ifndef __QNXNTO__
    mtimeNs = st.st_mtim.tv_nsec;
    ctimeNs = st.st_ctim.tv_nsec;
#endif
    char line[200];
    snprintf(line, sizeof line, " %o %lld %llu %lld.%09ld %lld.%09ld\n", (unsigned)st.st_mode, (long long)st.st_size,
             (unsigned long long)st.st_ino, (long long)st.st_mtime, mtimeNs, (long long)st.st_ctime, ctimeNs);
    *out += path + line;
    if (!S_ISDIR(st.st_mode)) return;
    std::vector<std::string> names;
    CHECK(fs::listDir(path, &names, 0));
    for (size_t i = 0; i < names.size(); ++i) addToSnapshot(path + "/" + names[i], out);
}

// Everything in the head unit's folder but the run folder. The folder itself is left out: the run folder is in
// it here, and on a head unit is not.
static std::string flashOf(const std::string& root) {
    std::vector<std::string> names;
    CHECK(fs::listDir(root, &names, 0));
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] != "run") addToSnapshot(root + "/" + names[i], &out);
    }
    return out;
}

static void quietStart(const std::string& name, const std::string& before, const char* what) {
    {
        Bench b(name, true);
        Phone* p = b.phone();
        CHECK(Parsed(b.call(p, "info.get", "{}")).result.isObject());
        CHECK(Parsed(b.call(p, "job.status", "{\"after\":0}")).result.isObject());
        CHECK(Parsed(b.call(p, "packages.list", "{}")).result.isObject());
    }
    std::string after = flashOf(gWork + "/" + name);
    CHECK_IN(std::string(what) + "\n" + before + "---\n" + after, after == before);
}

static void testQuietStart() {
    std::string why;
    // A head unit with packages installed and a job on record.
    {
        Bench b("quiet-start");
        Phone* p = b.phone();
        install(b, p, kHello, kHelloAndRuntime);
        CHECK(b.agent.job().state() == "succeeded");
    }
    std::string root = gWork + "/quiet-start", before = flashOf(root);
    CHECK(has(before, root + "/state/job/job.json ") && has(before, root + "/staging/unpack ") && !has(before, root + "/run"));
    quietStart("quiet-start", before, "an agent that is started again");
    CHECK_IN(why, fs::removeTree(root + "/run", &why));
    quietStart("quiet-start", before, "the first agent after a power down");
    quietStart("quiet-start", before, "and the one after it");

    // One on which nothing was ever asked for.
    {
        Bench b("quiet-start-new");
    }
    root = gWork + "/quiet-start-new";
    before = flashOf(root);
    quietStart("quiet-start-new", before, "an agent that is started again, with no job on record");
    CHECK_IN(why, fs::removeTree(root + "/run", &why));
    quietStart("quiet-start-new", before, "the first agent after a power down, with no job on record");

    // The check itself: a start that has something to finish does write.
    {
        Bench b("quiet-start", true);
        Phone* p = b.phone();
        std::string planId = planOk(b, p, "org.nbtapp.demoplugin-1.0.0", "", "org.nbtapp.demoplugin-1.0.0");
        p->stallAt = 100;
        startJob(b, p, planId);
        CHECK(b.agent.job().fetching());
    }
    root = gWork + "/quiet-start";
    before = flashOf(root);
    {
        Bench b("quiet-start", true);
        CHECK(b.agent.job().state() == "failed");
    }
    CHECK(flashOf(root) != before);
}

// The process ends in the middle of an install step, as in a power cut. The next agent undoes the step and says so.
class DieAt : public CrashHook {
public:
    explicit DieAt(int count) : left(count) {}
    virtual void point(const char*) {
        if (--left == 0) _exit(42);
    }
    int left;
};

static void testCrashDuringJob() {
    // hello 1.2.0 over 1.1.0, with the runtime as a first step that completes. The child dies at the n-th point
    // of the job; n runs until a child gets through.
    int deaths = 0, undone = 0, finished = 0;
    for (int n = 1;; ++n) {
        std::string name = "crash-job";
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            Bench b(name);
            Phone* p = b.phone();
            install(b, p, kHelloOld, std::string(kHelloOld) + " " + kRuntime);
            std::string targets = "org.nbtapp.demoplugin-1.0.0 org.nbtapp.hello-1.2.0";
            std::string planId = planOk(b, p, targets, "", targets);
            DieAt die(n);
            b.agent.engine().setCrashHook(&die);
            startJob(b, p, planId);
            _exit(outcome(p) == "succeeded done done" ? 0 : 1);
        }
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
        if (WEXITSTATUS(status) == 0) break;
        CHECK_IN(num(n), WEXITSTATUS(status) == 42);
        ++deaths;

        Bench b(name, true);
        Phone* p = b.phone();
        Parsed st(b.call(p, "job.status", "{\"after\":0}"));
        json::Value job = st.result.find("job"), steps = job.find("result").find("steps");
        std::string plugin = str(steps.at(0), "result"), hello = str(steps.at(1), "result");
        std::string at = "death " + num(n) + ": " + str(job, "state") + " " + plugin + " " + hello;
        CHECK_IN(at, steps.size() == 2 && job.find("result").isObject());
        // Each step is whole or not there, and the report says which.
        CHECK_IN(at, (plugin == "done") == b.installed("org.nbtapp.demoplugin"));
        CHECK_IN(at, b.versionOf("org.nbtapp.hello") == (hello == "done" ? "1.2.0" : "1.1.0"));
        if (plugin == "failed") {
            CHECK_IN(at, str(steps.at(0).find("error"), "code") == "interrupted" && hello == "skipped" && str(job, "state") == "failed");
            ++undone;
        } else if (hello == "failed") {
            CHECK_IN(at, plugin == "done" && str(steps.at(1).find("error"), "code") == "interrupted" && str(job, "state") == "failed");
            ++undone;
        } else if (hello == "skipped") {
            // The first step was finished by the recovery; the second never began, and does not begin now.
            CHECK_IN(at, plugin == "done" && str(job, "state") == "failed");
            ++finished;
        } else {
            CHECK_IN(at, plugin == "done" && hello == "done" && str(job, "state") == "succeeded");
            ++finished;
        }
        json::Value events = st.result.find("events");
        CHECK_IN(at, events.size() >= 1 && str(events.at(events.size() - 1), "event") == "job.done");
        // The agent was started again on a head unit that was not, so what the plugin needs is still owed.
        if (plugin == "done") CHECK_IN(at, str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "hmi-restart");
        // And the head unit is usable: the job can be planned and run again.
        std::string again;
        if (plugin != "done") again += "org.nbtapp.demoplugin-1.0.0 ";
        if (hello != "done") again += "org.nbtapp.hello-1.2.0";
        if (!again.empty()) {
            p->events.clear();
            install(b, p, again, again);
        }
        CHECK(b.versionOf("org.nbtapp.hello") == "1.2.0" && b.installed("org.nbtapp.demoplugin"));
    }
    CHECK(deaths > 20 && undone > 10 && finished >= 2);
    printf("test_agent: a job survived the agent dying at %d points\n", deaths);
}

// ---- the agent's own update ---------------------------------------------------------------------------------------
// One agent places the new version and ends; "the launcher" here is the test, which renames the file as the
// launcher does and then starts an agent of the version the launcher would start, on the same folder.

static const char kAgentId[] = "org.nbtapp.agent";
static const char kAgentNext[] = "org.nbtapp.agent-1.1.0";
static const char kPluginHello[] = "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"plugin\",\"maxFrame\":65536,\"capabilities\":[]}";

static void put(const std::string& path, const std::string& text, unsigned mode = 0644) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::writeFile(path, text, mode, &why));
}

// The agent's own folder as hu_install.sh leaves one, with `version` as the one the starter starts.
static void makeHome(const std::string& root, const std::string& version) {
    put(root + "/agent/" + version + "/nbtagent", "#!/bin/sh\necho \"nbtagent " + version + " (build stand-in)\"\n", 0755);
    put(root + "/agent/" + version + "/nbtlaunch.sh", "#!/bin/sh\nexec \"${0%/*}/nbtagent\" \"$@\"\n", 0755);
    put(root + "/agent/current", version + "\n");
}

static std::string homeFile(const std::string& root, const char* name) {
    std::string path = root + "/agent/" + name;
    return fs::kindOf(path) == fs::kFile ? testutil::readFile(path) : std::string("(none)");
}

static void launcherRenames(const std::string& root) {
    CHECK(rename((root + "/agent/trial").c_str(), (root + "/agent/trial.started").c_str()) == 0);
}

// Plans the update to 1.1.0 and starts it; returns the plan's id. The job is left as it is when nothing moves.
static std::string startUpdate(Bench& b, Phone* p) {
    p->events.clear();
    std::string planId = planOk(b, p, kAgentNext, "", kAgentNext);
    Parsed started(startJob(b, p, planId));
    CHECK_IN(started.code(), started.result.isObject());
    return planId;
}

// Dies at the first point of that name.
class DieAtPoint : public CrashHook {
public:
    explicit DieAtPoint(const char* name) : name_(name) {}
    virtual void point(const char* name) {
        if (name_ == name) _exit(42);
    }

private:
    std::string name_;
};

static void testAgentUpdate() {
    std::string why;

    // What a plan says of the agent's own package.
    {
        Bench b("update-plans");
        Phone* p = b.phone();
        // An agent that nothing would start again: no folder of its versions.
        planRefused(b, p, planParams(kAgentNext, "", kAgentNext), "no-launcher", kAgentId);
        makeHome(b.root, "1.0.0");
        // It is alone in its job,
        std::string both = std::string(kAgentNext) + " " + kRuntime;
        planRefused(b, p, planParams(both, "", both), "conflict", kAgentId);
        // has to place its program,
        planRefused(b, p, planParams("org.nbtapp.agent-2.2.0", "", "org.nbtapp.agent-2.2.0"), "bad-manifest", kAgentId);
        // and comes after the version that runs, of which there is no record.
        std::string text = b.call(p, "job.plan", planParams(kAgentNext, "", kAgentNext));
        Parsed plan(text);
        json::Value steps = plan.result.find("steps");
        CHECK_IN(text, plan.result.find("planId").isString() && steps.size() == 1 && str(plan.result, "restart") == "none");
        CHECK_IN(text, str(steps.at(0), "action") == "upgrade" && str(steps.at(0), "id") == kAgentId &&
                           str(steps.at(0), "version") == "1.1.0" && str(steps.at(0), "oldVersion") == "1.0.0");
        Bench newer("update-plans-newer", false, false, "1.1.0");
        makeHome(newer.root, "1.1.0");
        planRefused(newer, newer.phone(), planParams(kAgentNext, "", kAgentNext), "already-installed", kAgentId);
    }

    // An update that works. First the agent that places the new version, and ends.
    const std::string root = gWork + "/update";
    std::string planId;
    {
        Bench b("update");
        makeHome(root, "1.0.0");
        Phone* p = b.phone();
        Phone* watcher = b.phone("2");
        Phone* far = b.phone("3", 4096, "serial");   // a phone on its own link, which only watches
        planId = startUpdate(b, p);
        CHECK(b.agent.job().restarting() && b.agent.job().handingOver() && !b.agent.busy() && !b.agent.onTrial());
        CHECK(b.agent.restartDue());                       // the links are written out: the process may end
        CHECK_IN(outcome(p), outcome(p) == "no job.done");
        Parsed progress(lastEvent(p, "job.progress"));
        CHECK(str(progress.params, "state") == "restarting" && str(progress.params, "id") == kAgentId && !progress.params.find("phase").present());
        CHECK(lastEvent(watcher, "job.progress") == lastEvent(p, "job.progress"));
        CHECK_IN(lastEvent(p, "job.log"), has(lastEvent(p, "job.log"), "ask for the job's status"));
        Parsed status(b.call(p, "job.status", "{}"));
        CHECK(str(status.result.find("job"), "state") == "restarting" && !status.result.find("job").find("result").present());
        // The new version is in its folder and handed to the launcher; nothing is decided.
        CHECK(homeFile(root, "trial") == "1.1.0\n" && homeFile(root, "current") == "1.0.0\n" && !b.installed(kAgentId));
        CHECK(fs::kindOf(root + "/agent/1.1.0/nbtagent") == fs::kFile && fs::kindOf(root + "/agent/1.0.0/nbtagent") == fs::kFile);
        // And nothing else begins.
        CHECK(Parsed(b.call(p, "job.plan", planParams(kRuntime, "", kRuntime))).code() == "busy");
        CHECK(Parsed(b.call(p, "system.restart", "{\"kind\":\"hmi\"}")).code() == "busy");
        CHECK(Parsed(b.call(p, "job.cancel", "{\"planId\":\"" + planId + "\"}")).code() == "too-late");
        // The process ends now. A phone on its own link has heard that the job is restarting, and is told last
        // that its link closes for that: it ends the link, and connects again to the version that starts.
        CHECK(lastEvent(far, "job.progress") == lastEvent(p, "job.progress") && !far->closed);
        b.agent.endPhoneLinks(kClosingRestarting, 2000);
        far->pump();
        CHECK(far->closed && closingReason(far) == "restarting" && !p->closed && !watcher->closed && b.agent.job().restarting());
    }

    // The launcher starts the new version, on trial. It answers, and writes nothing that the old version reads.
    launcherRenames(root);
    std::string flash = flashOf(root);
    {
        Bench b("update", true, false, "1.1.0");
        CHECK(b.agent.onTrial() && b.agent.job().restarting() && b.agent.job().planId() == planId && !b.agent.restartDue());
        // The plugin is not what this version waits for: an agent that no phone can reach would answer it too.
        Phone* plugin = b.connect();
        CHECK(Parsed(b.call(plugin, "hello", kPluginHello)).result.isObject());
        CHECK(str(Parsed(b.call(plugin, "info.get", "{}")).result, "agentVersion") == "1.1.0");
        CHECK(str(Parsed(b.call(plugin, "job.status", "{\"after\":0}")).result.find("job"), "state") == "restarting");
        CHECK(Parsed(b.call(plugin, "packages.list", "{}")).result.find("packages").size() == 0);
        CHECK(Parsed(b.call(plugin, "job.plan", planParams(kRuntime, "", kRuntime))).code() == "busy");
        CHECK(Parsed(b.call(plugin, "system.restart", "{\"kind\":\"hmi\"}")).code() == "busy");
        CHECK(b.agent.onTrial() && plugin->events.empty());
        CHECK_IN(flash + "---\n" + flashOf(root), flashOf(root) == flash);

        // A phone connects again and asks how the job went. That is the confirmation, and its answer has the end.
        Phone* p = b.phone();
        std::string text = b.call(p, "job.status", "{\"planId\":\"" + planId + "\",\"after\":0}");
        Parsed status(text);
        json::Value job = status.result.find("job"), steps = job.find("result").find("steps");
        CHECK_IN(text, str(job, "state") == "succeeded" && steps.size() == 1 && str(steps.at(0), "result") == "done" &&
                           str(steps.at(0), "id") == kAgentId && str(steps.at(0), "version") == "1.1.0");
        CHECK(!b.agent.onTrial() && homeFile(root, "current") == "1.1.0\n" && homeFile(root, "trial.started") == "(none)");
        CHECK(b.versionOf(kAgentId) == "1.1.0" && fs::kindOf(root + "/agent/1.0.0/nbtagent") == fs::kFile);
        // Every controller hears it, numbered above whatever the old version sent; and the job's log has both
        // versions' lines.
        CHECK_IN(outcome(plugin), outcome(plugin) == "succeeded done" && outcome(p) == "succeeded done");
        CHECK(integer(Parsed(lastEvent(plugin, "job.done")).root, "seq") > 1000000);
        json::Value events = status.result.find("events");
        CHECK(events.size() > 4 && str(events.at(events.size() - 1), "event") == "job.done");
        CHECK_IN(text, has(text, "ask for the job's status") && has(text, "a controller has reached the new version of the agent"));
        std::vector<std::string> left;
        CHECK(fs::listDir(b.agent.engine().store().blobDir(), &left, 0) && left.empty());
        // From here on it is an agent like any other.
        CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "restartNeeded") == "none");
        install(b, p, kHello, kHelloAndRuntime);
    }
    // And one whose next start has nothing to finish, and writes nothing.
    flash = flashOf(root);
    {
        Bench b("update", true, false, "1.1.0");
        Phone* p = b.phone();
        CHECK(!b.agent.onTrial() && b.agent.job().state() == "succeeded" && b.versionOf(kAgentId) == "1.1.0");
        CHECK(Parsed(b.call(p, "job.status", "{\"after\":0}")).result.isObject());
    }
    CHECK_IN(flash + "---\n" + flashOf(root), flashOf(root) == flash);

    // An update that nobody confirms. The new version runs, a link opens, and nobody asks for the job.
    const std::string back = gWork + "/update-back";
    {
        Bench b("update-back");
        makeHome(back, "1.0.0");
        startUpdate(b, b.phone());
        CHECK(b.agent.job().restarting());
    }
    launcherRenames(back);
    {
        Bench b("update-back", true, false, "1.1.0");
        Phone* plugin = b.connect();
        CHECK(Parsed(b.call(plugin, "hello", kPluginHello)).result.isObject() && b.agent.onTrial());
    }
    CHECK_IN(why, fs::removeTree(back + "/run", &why));     // the head unit is turned off
    {
        // The launcher finds trial.started and starts the version in current, which undoes the update.
        Bench b("update-back", true);
        CHECK(!b.agent.onTrial() && b.agent.job().state() == "failed");
        Phone* p = b.phone();
        std::string text = b.call(p, "job.status", "{\"after\":0}");
        Parsed status(text);
        json::Value job = status.result.find("job"), steps = job.find("result").find("steps");
        CHECK_IN(text, str(job, "state") == "failed" && steps.size() == 1 && str(steps.at(0), "result") == "failed" &&
                           str(steps.at(0).find("error"), "code") == "not-confirmed");
        json::Value events = status.result.find("events");
        CHECK(events.size() >= 2 && str(events.at(events.size() - 1), "event") == "job.done" && has(text, "the update of the agent was undone"));
        CHECK(homeFile(back, "trial.started") == "(none)" && homeFile(back, "current") == "1.0.0\n" && !b.installed(kAgentId));
        CHECK(fs::kindOf(back + "/agent/1.1.0") == fs::kNone && fs::kindOf(back + "/agent/1.0.0/nbtagent") == fs::kFile);
        CHECK(str(Parsed(b.call(p, "info.get", "{}")).result, "agentVersion") == "1.0.0");
        // The update is offered again, and runs again.
        startUpdate(b, p);
        CHECK(b.agent.job().restarting() && homeFile(back, "trial") == "1.1.0\n");
    }

    // The new version stops between the rename that confirms it and the step's commit line. The launcher starts
    // it again from current, and it finishes by itself: nobody has to ask a second time.
    launcherRenames(back);
    {
        fflush(0);
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            Bench b("update-back", true, false, "1.1.0");
            DieAtPoint die("confirm");
            b.agent.engine().setCrashHook(&die);
            Phone* p = b.phone();
            b.call(p, "job.status", "{}");
            _exit(0);
        }
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    }
    CHECK(homeFile(back, "current") == "1.1.0\n" && homeFile(back, "trial.started") == "1.1.0\n");
    {
        Bench b("update-back", true, false, "1.1.0");
        CHECK(!b.agent.onTrial() && b.agent.job().state() == "succeeded" && b.versionOf(kAgentId) == "1.1.0");
        CHECK(homeFile(back, "trial.started") == "(none)");
        Phone* p = b.phone();
        std::string text = b.call(p, "job.status", "{\"after\":0}");
        Parsed status(text);
        CHECK_IN(text, str(status.result.find("job"), "state") == "succeeded" && has(text, "the update of the agent is complete"));
        json::Value events = status.result.find("events");
        CHECK(str(events.at(events.size() - 1), "event") == "job.done");
    }

    // A version that was put into the folder by hand and named in `trial`: no job placed it and no step waits.
    // It is on trial all the same, and stays once a controller has asked for the job, though there is none.
    const std::string byHand = gWork + "/update-by-hand";
    {
        Bench b("update-by-hand");
    }
    makeHome(byHand, "1.0.0");
    put(byHand + "/agent/1.1.0/nbtagent", "#!/bin/sh\necho \"nbtagent 1.1.0 (build stand-in)\"\n", 0755);
    put(byHand + "/agent/trial.started", "1.1.0\n");
    {
        Bench b("update-by-hand", true, false, "1.1.0");
        Phone* p = b.phone();
        CHECK(b.agent.onTrial() && !b.agent.job().exists());
        CHECK(Parsed(b.call(p, "job.plan", planParams(kRuntime, "", kRuntime))).code() == "busy");
        Parsed status(b.call(p, "job.status", "{}"));
        CHECK(status.result.isObject() && !status.result.find("job").present());
        CHECK(!b.agent.onTrial() && homeFile(byHand, "current") == "1.1.0\n" && homeFile(byHand, "trial.started") == "(none)");
        CHECK(fs::kindOf(byHand + "/agent/1.0.0/nbtagent") == fs::kFile && !b.installed(kAgentId));
        install(b, p, kHello, kHelloAndRuntime);
    }

    // A new version that does not say it is the version its package names never gets a try: the step fails like
    // any other, and the agent goes on.
    {
        Bench b("update-misnamed");
        makeHome(b.root, "1.0.0");
        Phone* p = b.phone();
        startJob(b, p, planOk(b, p, "org.nbtapp.agent-2.0.0", "", "org.nbtapp.agent-2.0.0"));
        CHECK_IN(outcome(p), outcome(p) == "failed failed(not-confirmed)");
        CHECK(!b.agent.job().handingOver() && !b.agent.restartDue() && homeFile(b.root, "trial") == "(none)");
        CHECK(fs::kindOf(b.root + "/agent/2.0.0") == fs::kNone);
        install(b, p, kHello, kHelloAndRuntime);
    }

    // The new version cannot be handed to the launcher: the step is undone there and then, and the agent stays.
    {
        Bench b("update-not-handed");
        makeHome(b.root, "1.0.0");
        CHECK_IN(why, fs::makeDirs(b.root + "/agent/trial", &why));   // a folder where the file would go
        Phone* p = b.phone();
        startUpdate(b, p);
        CHECK_IN(outcome(p), outcome(p) == "failed failed(io-error)");
        CHECK(!b.agent.job().handingOver() && !b.agent.restartDue() && fs::kindOf(b.root + "/agent/1.1.0") == fs::kNone);
        std::vector<std::string> names;
        CHECK(fs::listDir(b.agent.engine().store().journalDir(), &names, 0) && names.empty());
        install(b, p, kHello, kHelloAndRuntime);
    }
}

// ---- the session that is written down -----------------------------------------------------------------------------

static void touch(const std::string& path, long seconds) {
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = seconds;
    times[0].tv_usec = times[1].tv_usec = 0;
    CHECK_IN(path, utimes(path.c_str(), times) == 0);
}

static std::string listParams(const std::string& package) { return "{\"package\":\"" + package + "\",\"area\":\"logs\"}"; }

// files.fetch of one path; `more` is the rest of the params, after the path.
static std::string fetchParams(const std::string& package, const std::string& path, const std::string& more = ",\"compress\":\"deflate\"") {
    return "{\"package\":\"" + package + "\",\"area\":\"logs\",\"path\":\"" + path + "\"" + more + "}";
}


static std::string sessionText() {
    Bench b("session");
    b.platform.fakeFree = 500000000;
    // The car's screen: a controller on the local socket, whose link is not written down.
    Phone* screen = b.connect();
    CHECK(Parsed(b.hello(screen, kGoodHello)).result.isObject());
    Phone* p = b.connect("serial");
    p->record = true;
    p->maxFrame = 4096;
    for (std::map<std::string, Pkg>::const_iterator it = gPkgs.begin(); it != gPkgs.end(); ++it) {
        p->blobs[it->second.sha256] = it->second.bytes;
        p->blobs[it->second.manifestSha256] = it->second.manifest;
    }
    b.call(p, "info.get", "{}");
    b.hello(p, "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":\"5f2c9a1e07b34d6688a0c1d2e3f40516\","
               "\"name\":\"Pixel 9\",\"maxFrame\":4096,\"capabilities\":[]}");
    // Not paired yet: nothing but ping is answered, and there is no key to be had until the car's screen shows
    // a code.
    b.call(p, "info.get", "{}");
    b.call(p, "ping", "{}");
    b.call(p, "pairing.issue", "{\"code\":\"000000\"}");
    std::string code = str(Parsed(b.call(screen, "pairing.open", "{}")).result, "code");
    b.call(p, "pairing.issue", codeParams(wrongCode(code)));
    Parsed issued(b.call(p, "pairing.issue", codeParams(code)));
    uint8_t key[kPairingKeyBytes];
    CHECK(fromHex(str(issued.result, "key"), key, sizeof key) && pairingClosed(screen) == "issued");
    // auth, with the key that came.
    CHECK(Parsed(b.authenticate(p, std::string((const char*)key, sizeof key), "5a0f3c9d2e7b4a61c8d5e2f7a4b1c6d3")).result.isObject());
    b.call(p, "info.get", "{}");
    b.call(p, "packages.list", "{}");
    b.call(p, "sources.list", "{}");
    b.call(p, "source.add", "{\"name\":\"Somebody else\",\"key\":\"" + toHex(gOtherKey.key, 32) + "\"}");
    b.call(p, "source.remove", "{\"key\":\"" + toHex(gPackageKey.key, 32) + "\"}");
    b.call(p, "ping", "{}");
    b.call(p, "job.status", "{}");
    // A plan that cannot run, and one that can.
    b.call(p, "job.plan", planParams(kHello, "org.nbtapp.old", kHello));
    Parsed plan(b.call(p, "job.plan", planParams("org.nbtapp.runtime-2.1.0", "", "org.nbtapp.runtime-2.1.0")));
    std::string planId = str(plan.result, "planId");
    startJob(b, p, planId);
    b.call(p, "job.status", "{\"planId\":\"" + planId + "\",\"after\":0}");
    b.call(p, "packages.list", "{}");
    b.call(p, "job.cancel", "{\"planId\":\"" + planId + "\"}");
    // And out again.
    Parsed removal(b.call(p, "job.plan", planParams("", "org.nbtapp.runtime!", "")));
    startJob(b, p, str(removal.result, "planId"));
    // The agent's own log: listed, and sent deflated; and a files.cancel of that stream, which has ended by then.
    {
        std::string own = b.root + "/run/log/agent.log", text;
        for (int i = 0; i < 40; ++i) text += "a line of the agent's log, " + num(i) + " of 40, as such a line might read\n";
        put(own, text);
        touch(own, 1794400000);
        b.agent.setOwnLog(own);
        b.call(p, "files.list", listParams(kAgentId));
        Parsed fetched(b.call(p, "files.fetch", fetchParams(kAgentId, "agent.log")));
        b.call(p, "files.cancel", "{\"stream\":" + num(integer(fetched.result, "stream")) + "}");
    }
    b.call(p, "system.restart", "{\"kind\":\"hmi\"}");
    // The phone's pairing is ended on the car's screen: the agent says that it closes the link, and why, and
    // closes it.
    CHECK(integer(Parsed(b.call(screen, "phones.forget", "{\"all\":true}")).result, "forgotten") == 1);
    CHECK(p->closed && closingReason(p) == "forgotten");

    return "# One conversation between a phone and the agent, frame by frame (see the plan document, \"Phone and agent\n"
           "# protocol\" and its \"Message reference\" tab). tests/agent/test_agent.cpp writes this file and fails when the\n"
           "# agent no longer says exactly this; the Android app's tests read it to learn what the agent sends.\n"
           "#\n"
           "# The link is a Bluetooth serial one, so each side gives 4,096 bytes as its largest frame, and the phone\n"
           "# has to be paired and to do `auth` before the agent tells it anything. It is paired here: before the\n"
           "# pairing.issue that is answered `rejected`, a controller on the head unit's local socket, which is not\n"
           "# in this file, opened the pairing window, and the code it was given is the one in the pairing.issue\n"
           "# after that. The key in that answer is the K of the two proofs of `auth`; tests/test_protocol_vectors.py\n"
           "# works both out again from this file. At the end the same controller ends the phone's pairing: the\n"
           "# agent's last frame is the event link.closing, with which it says that it closes the link and why, and\n"
           "# a phone app that reads it closes its side of the link at once.\n"
           "#\n"
           "#   magic HEX   the bytes the phone, which dialled, sends before its first frame\n"
           "#   > HEX       a frame from the phone to the agent\n"
           "#   < HEX       a frame from the agent to the phone\n"
           "#\n"
           "# A line that starts with # shows the next frame's payload if it is a message. The phone's lines are examples\n"
           "# of what the agent accepts; another client may order the members of its messages differently. The agent's\n"
           "# lines are what this agent sends when it is in the same state: a head unit with nothing installed, the\n"
           "# test key as its only package key, 500,000,000 bytes free everywhere, the head-unit id, plan ids, pairing\n"
           "# code, pairing key and nonce that the test's counter gives, and a clock that stands still, so\n"
           "# job.progress comes only when something changes and no ping is due. The package is\n"
           "# tests/packages/runtime-2.1.0 as tests/make_packages.py builds it. Near the end the phone pulls the\n"
           "# agent's own log, a file of 40 lines the test writes, with files.list and files.fetch: the DATA frames\n"
           "# from the agent are one raw deflate stream (RFC 1951), as the zlib of the test bench's image makes it.\n\n"
           "magic 4e42544c\n\n" +
           p->transcript;
}

// ---- starting with the head unit ----------------------------------------------------------------------------------

static bool flag(json::Value object, const char* name) {
    json::Value v = object.find(name);
    return v.isBool() && v.boolean();
}

// A package its author signed with a key of their own, offered with the catalogue's signature over its manifest.
static void testDetachedSignature() {
    const char kAuthors[] = "detached/author-signed";
    std::string why;

    // The bench trusts the package key only. The archive's manifest.sig is the other key's, so before agents took
    // the offer's signature the plan passed and the install was refused with bad-signature.
    {
        Bench b("detached");
        Phone* p = b.phone();
        Parsed hello(p->helloReply);
        CHECK_IN(p->helloReply, hello.result.find("capabilities").size() == 3 &&
                                    hello.result.find("capabilities").at(1).string() == kCapabilityDetachedSignature);
        // First the version before it, signed in the usual way: one key then verifies both, so the upgrade's key
        // matches the installed one.
        install(b, p, kHelloOld, std::string(kHelloOld) + " " + kRuntime);
        p->events.clear();
        std::string planId = planOk(b, p, kAuthors, "", kAuthors);
        startJob(b, p, planId);
        CHECK_IN(outcome(p), outcome(p) == "succeeded done");
        CHECK(b.versionOf("org.nbtapp.hello") == "1.2.0" && fs::kindOf(b.app("org.nbtapp.hello") + "/bin/hello") == fs::kFile);
        // The installed record names the key that verified the planned signature: the package key, which is
        // built in, as the reserved org.nbtapp. ids need.
        InstalledPackage record;
        CHECK_IN(why, b.agent.engine().store().load("org.nbtapp.hello", &record, &why));
        CHECK(record.key == toHex(gPackageKey.key, sizeof gPackageKey.key));
        // The job's record keeps the signature with its step, so an agent that starts again mid-job checks the same.
        std::string job;
        CHECK_IN(why, fs::readFile(b.root + "/state/job/job.json", 65536, &job, &why));
        CHECK_IN(job, has(job, "\"signature\":\"" + pkg(kAuthors).signature + "\""));
    }

    // The same archive offered with its own manifest.sig: a key the agent does not trust, refused at the plan.
    {
        Bench b("detached-own");
        Phone* p = b.phone();
        Pkg own = pkg(kAuthors);
        MemorySource source(own.bytes.data(), own.bytes.size());
        Package package;
        Error error;
        std::vector<TrustedKey> other(1, gOtherKey);
        CHECK_IN(error.message, package.open(&source, other, &error));
        own.signature = toHex((const uint8_t*)package.signatureBytes().data(), package.signatureBytes().size());
        std::string params = "{\"install\":[{\"id\":\"org.nbtapp.hello\",\"version\":\"1.2.0\"}],\"remove\":[],\"offers\":[" +
                             offer(own) + "]}";
        planRefused(b, p, params, "bad-signature", "org.nbtapp.hello");
        CHECK(!b.installed("org.nbtapp.hello"));
    }

    // A planned signature that does not verify at install is refused there too: the step reads the one it was given.
    {
        Bench b("detached-step");
        Phone* p = b.phone();
        install(b, p, kRuntime, kRuntime);   // hello needs it
        std::string archive = b.root + "/author-signed.nbtpkg";
        CHECK_IN(why, fs::makeDirs(b.root, &why) && fs::writeFile(archive, pkg(kAuthors).bytes, 0600, &why));
        std::string flipped = pkg(kAuthors).signature;
        flipped[20] = flipped[20] == '0' ? '1' : '0';
        StepReport report = b.agent.engine().install(archive, pkg(kAuthors).manifestSha256, 0, flipped);
        CHECK_IN(report.error.message, report.error.code == "bad-signature" && !b.installed("org.nbtapp.hello"));
        report = b.agent.engine().install(archive, pkg(kAuthors).manifestSha256, 0, "not hex");
        CHECK_IN(report.error.message, report.error.code == "bad-signature" && !b.installed("org.nbtapp.hello"));
        // Without one the archive's own manifest.sig is read, and the bench does not trust its key.
        report = b.agent.engine().install(archive, pkg(kAuthors).manifestSha256, 0);
        CHECK_IN(report.error.message, report.error.code == "bad-signature" && !b.installed("org.nbtapp.hello"));
        report = b.agent.engine().install(archive, pkg(kAuthors).manifestSha256, 0, pkg(kAuthors).signature);
        CHECK_IN(report.error.message, report.error.code.empty() && b.versionOf("org.nbtapp.hello") == "1.2.0");
    }
}

static void testAutostart() {
    const char kPlugin[] = "org.nbtapp.plugin";
    const std::string on = "{\"on\":true}", off = "{\"on\":false}";
    std::string why;

    // An agent that no starter starts: it cannot be started with the head unit either, and says so.
    {
        Bench b("autostart-nowhere", false, false, "1.1.0");
        Phone* p = b.phone();
        Parsed hello(p->helloReply);
        CHECK_IN(p->helloReply, hello.result.find("capabilities").size() == 3 &&
                                    hello.result.find("capabilities").at(0).string() == kCapabilityAutostart);
        Parsed state(b.call(p, "autostart.get", "{}"));
        CHECK(state.result.size() == 4 && !flag(state.result, "on") && !flag(state.result, "available"));
        CHECK(str(state.result, "reason") == "no-launcher" && !state.result.find("needs").present() && integer(state.result, "starts") == 0);
        CHECK(Parsed(b.call(p, "autostart.set", on)).code() == "no-launcher");
        CHECK(Parsed(b.call(p, "autostart.set", off)).result.isObject() && fs::kindOf(b.root + "/agent") == fs::kNone);
    }

    Bench b("autostart", false, false, "1.1.0");
    makeHome(b.root, "1.1.0");
    put(b.root + "/agent/launcher", "#!/bin/sh\n", 0755);
    const std::string wanted = b.root + "/agent/" + kAutostartSwitch, record = b.root + "/agent/" + kAutostartBoots;
    Phone* p = b.phone();

    // The iDrive plugin is what does the starting. With none installed there is nothing to switch on.
    {
        Parsed state(b.call(p, "autostart.get", "{}"));
        CHECK(!flag(state.result, "on") && !flag(state.result, "available") && str(state.result, "reason") == "plugin-missing");
        CHECK(str(state.result.find("needs"), "id") == kPlugin && str(state.result.find("needs"), "version") == kAutostartPluginRange);
        Parsed refused(b.call(p, "autostart.set", on));
        CHECK(refused.code() == "plugin-missing" && has(refused.error.find("message").string(), kPlugin));
        CHECK(fs::kindOf(wanted) == fs::kNone);
        // Off is what it is, and asking for it is no fault.
        Parsed same(b.call(p, "autostart.set", off));
        CHECK(same.result.isObject() && !flag(same.result, "on") && fs::kindOf(wanted) == fs::kNone);
    }
    const char* bad[] = {"{}", "{\"on\":1}", "{\"on\":\"yes\"}", "{\"on\":true,\"now\":true}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK_IN(bad[i], Parsed(b.call(p, "autostart.set", bad[i])).code() == "bad-request");

    // A plugin from before the versions that start the agent is not one either.
    install(b, p, "org.nbtapp.plugin-0.9.0", "org.nbtapp.plugin-0.9.0");
    {
        Parsed state(b.call(p, "autostart.get", "{}"));
        CHECK(!flag(state.result, "available") && str(state.result, "reason") == "plugin-too-old" &&
              str(state.result.find("needs"), "version") == kAutostartPluginRange);
        Parsed refused(b.call(p, "autostart.set", on));
        CHECK(refused.code() == "plugin-too-old" && has(refused.error.find("message").string(), "0.9.0"));
    }

    // With one that does: switched on, which is the file, and the starts on record from before are forgotten.
    install(b, p, "org.nbtapp.plugin-1.0.0", "org.nbtapp.plugin-1.0.0");
    put(record, "315532800\nnot a start\n315532900\n");
    {
        Parsed state(b.call(p, "autostart.get", "{}"));
        CHECK(state.result.size() == 3 && flag(state.result, "available") && !flag(state.result, "on") && integer(state.result, "starts") == 2);
        Parsed set(b.call(p, "autostart.set", on));
        CHECK(set.result.size() == 3 && flag(set.result, "on") && flag(set.result, "available") && integer(set.result, "starts") == 0);
        CHECK(testutil::readFile(wanted) == "on\n" && fs::kindOf(record) == fs::kNone);
        CHECK(flag(Parsed(b.call(p, "autostart.set", on)).result, "on") && testutil::readFile(wanted) == "on\n");   // on already
    }

    // The plugin, which starts the agent, may ask how things stand and is not the one to change them.
    Phone* plugin = b.connect();
    CHECK(Parsed(b.call(plugin, "hello", kPluginHello)).result.isObject());
    CHECK(flag(Parsed(b.call(plugin, "autostart.get", "{}")).result, "on"));
    CHECK(Parsed(b.call(plugin, "autostart.set", off)).code() == "forbidden" && fs::kindOf(wanted) == fs::kFile);

    // A phone on a link of its own, paired, switches it off again, and a controller at the console on once more.
    Phone* far = b.phone("2", 4096, "serial");
    {
        Parsed set(b.call(far, "autostart.set", off));
        CHECK(set.result.isObject() && !flag(set.result, "on") && flag(set.result, "available") && fs::kindOf(wanted) == fs::kNone);
    }
    Phone* cli = b.connect();
    CHECK(Parsed(b.call(cli, "hello", kGoodHello)).result.isObject());
    CHECK(flag(Parsed(b.call(cli, "autostart.set", on)).result, "on") && fs::kindOf(wanted) == fs::kFile);

    // Without the plugin it stays as it was set, and says that nothing would do the starting.
    {
        std::string planId = Parsed(b.call(p, "job.plan", planParams("", kPlugin, ""))).result.find("planId").string();
        p->events.clear();
        startJob(b, p, planId);
        CHECK_IN(outcome(p), has(outcome(p), "succeeded"));
        Parsed state(b.call(p, "autostart.get", "{}"));
        CHECK(flag(state.result, "on") && !flag(state.result, "available") && str(state.result, "reason") == "plugin-missing");
        CHECK(flag(Parsed(b.call(p, "autostart.set", off)).result, "on") == false && fs::kindOf(wanted) == fs::kNone);
    }

    // A version on trial writes nothing into the agent's folder, this file included.
    {
        Bench t("autostart-trial", false, false, "1.2.0");
        makeHome(t.root, "1.1.0");
        put(t.root + "/agent/1.2.0/nbtagent", "#!/bin/sh\n", 0755);
        put(t.root + "/agent/launcher", "#!/bin/sh\n", 0755);
        put(t.root + "/agent/trial.started", "1.2.0\n");
    }
    {
        Bench t("autostart-trial", true, false, "1.2.0");
        Phone* plug = t.connect();
        CHECK(Parsed(t.call(plug, "hello", kPluginHello)).result.isObject() && t.agent.onTrial());
        CHECK(Parsed(t.call(plug, "autostart.get", "{}")).result.isObject());
        Phone* q = t.connect();
        CHECK(Parsed(t.call(q, "hello", kGoodHello)).result.isObject());
        CHECK(Parsed(t.call(q, "autostart.set", off)).code() == "busy" && t.agent.onTrial());
    }
}

// ---- files: a package's logs, pulled ------------------------------------------------------------------------------

static const char kLogger[] = "org.nbtapp.logger-1.0.0";
static const char kLoggerId[] = "org.nbtapp.logger";

// A raw deflate stream inflated with zlib itself. The test fails if it is not one whole stream and nothing more.
static std::string inflated(const std::string& packed) {
    z_stream z;
    memset(&z, 0, sizeof z);
    CHECK(inflateInit2(&z, -MAX_WBITS) == Z_OK);
    std::string out;
    z.next_in = (Bytef*)packed.data();
    z.avail_in = (uInt)packed.size();
    int rc = Z_OK;
    while (rc == Z_OK) {
        unsigned char buffer[65536];
        z.next_out = buffer;
        z.avail_out = sizeof buffer;
        rc = inflate(&z, Z_NO_FLUSH);
        out.append((const char*)buffer, sizeof buffer - z.avail_out);
        if (rc == Z_BUF_ERROR && z.avail_in == 0) break;
    }
    CHECK_IN("inflate: " + num(rc), rc == Z_STREAM_END && z.avail_in == 0);
    inflateEnd(&z);
    return out;
}

// files.fetch, served to its end. Returns the reply; *bytes is the file as the stream gave it.
static std::string fetched(Bench& b, Phone* p, const std::string& params, std::string* bytes) {
    std::string text = b.call(p, "files.fetch", params);
    Parsed r(text);
    bytes->clear();
    if (!r.result.isObject()) return text;
    if (!r.result.find("stream").present()) {
        CHECK_IN(text, integer(r.result, "size") == 0);
        return text;
    }
    unsigned stream = (unsigned)integer(r.result, "stream");
    CHECK_IN(text, p->ended.count(stream) == 1);
    *bytes = str(r.result, "compress") == "deflate" ? inflated(p->received[stream]) : p->received[stream];
    CHECK_IN(text, (long long)bytes->size() == integer(r.result, "size"));
    return text;
}

// Where in what a phone heard an entry is, or -1.
static long position(const Phone* p, const std::string& what) {
    for (size_t i = 0; i < p->heard.size(); ++i) {
        if (p->heard[i] == what) return (long)i;
    }
    return -1;
}

// Turns of the agent while the phone only writes.
static void turns(Bench& b, Phone* p, int count) {
    for (int i = 0; i < count; ++i) {
        p->pump();
        b.agent.turn(0);
    }
}

static void testFiles() {
    Bench b("files");
    Phone* p = b.phone();
    // Nothing installed, and requests that are no use.
    CHECK(Parsed(b.call(p, "files.list", listParams(kLoggerId))).code() == "not-found");
    CHECK(Parsed(b.call(p, "files.list", "{\"package\":\"org.nbtapp.logger\",\"area\":\"data\"}")).code() == "unsupported");
    CHECK(Parsed(b.call(p, "files.list", "{\"package\":\"org.nbtapp.logger\"}")).code() == "bad-request");
    CHECK(Parsed(b.call(p, "files.list", "{\"package\":\"../state\",\"area\":\"logs\"}")).code() == "bad-request");
    const char* badFetch[] = {"", ",\"compress\":\"zstd\"", ",\"compress\":\"none\",\"offset\":-1", ",\"compress\":\"none\",\"length\":0",
                              ",\"compress\":\"none\",\"offset\":\"0\""};
    for (size_t i = 0; i < sizeof badFetch / sizeof badFetch[0]; ++i) {
        CHECK_IN(badFetch[i], Parsed(b.call(p, "files.fetch", fetchParams(kLoggerId, "${data}/log/logger.log", badFetch[i]))).code() == "bad-request");
    }

    install(b, p, kLogger, kLogger);
    std::string data = b.platform.dataDir(kLoggerId), tmp = b.platform.tmpDir(kLoggerId), fixed = b.root + "/system/tmp/logger.log";
    // ${tmp} was made for the package, and its hook found it in NBT_TMP_DIR.
    CHECK(fs::kindOf(tmp + "/crash.txt") == fs::kFile);
    // The third entry is outside the package's folders: the test puts a file there, and the times in order.
    put(fixed, "a log in a fixed place\n");
    touch(data + "/log/logger.log", 1794400300);
    touch(tmp + "/crash.txt", 1794400200);
    touch(data + "/log/older.log", 1794400100);
    touch(fixed, 1794400000);
    // A link among the logs, to the pairing keys: not listed, and not served.
    CHECK(fs::makeLink(b.platform.stateDir() + "/phones.json", data + "/log/keys.log", 0));

    {
        Parsed r(b.call(p, "files.list", listParams(kLoggerId)));
        json::Value files = r.result.find("files");
        std::string listed;
        for (size_t i = 0; i < files.size(); ++i) listed += str(files.at(i), "path") + " " + num(integer(files.at(i), "modified")) + "; ";
        CHECK_IN(listed, listed == "${data}/log/logger.log 1794400300; ${tmp}/crash.txt 1794400200; ${data}/log/older.log 1794400100; "
                                   "/tmp/logger.log 1794400000; ");
        CHECK(integer(files.at(1), "size") == 13 && integer(files.at(3), "size") == 23 && r.result.find("more").isBool() &&
              !r.result.find("more").boolean());
        // packages.list says which packages have logs to list.
        Parsed list(b.call(p, "packages.list", "{}"));
        CHECK(list.result.find("packages").size() == 1 && flag(list.result.find("packages").at(0), "logs"));
    }

    // Each file comes back as it is on disk: deflated, much smaller, and as it is.
    std::string log = testutil::readFile(data + "/log/logger.log"), bytes;
    {
        Parsed r(fetched(b, p, fetchParams(kLoggerId, "${data}/log/logger.log"), &bytes));
        CHECK(bytes == log && str(r.result, "compress") == "deflate" && log.size() > 200000);
        CHECK(p->received[(unsigned)integer(r.result, "stream")].size() * 5 < log.size());
        Parsed none(fetched(b, p, fetchParams(kLoggerId, "${data}/log/logger.log", ",\"compress\":\"none\""), &bytes));
        CHECK(bytes == log && str(none.result, "compress") == "none" && integer(none.result, "stream") == integer(r.result, "stream") + 1);
        fetched(b, p, fetchParams(kLoggerId, "/tmp/logger.log"), &bytes);
        CHECK(bytes == "a log in a fixed place\n");
        fetched(b, p, fetchParams(kLoggerId, "${tmp}/crash.txt", ",\"compress\":\"none\""), &bytes);
        CHECK(bytes == "crash report\n");
    }
    // A range; a length past the end stops at the end; an offset at the end has nothing to send and no stream; one
    // past it is out of range.
    fetched(b, p, fetchParams(kLoggerId, "${data}/log/logger.log", ",\"offset\":100,\"length\":50,\"compress\":\"deflate\""), &bytes);
    CHECK(bytes == log.substr(100, 50));
    fetched(b, p, fetchParams(kLoggerId, "${data}/log/logger.log", ",\"offset\":" + num((long long)log.size() - 10) + ",\"length\":1000,\"compress\":\"none\""), &bytes);
    CHECK(bytes == log.substr(log.size() - 10));
    unsigned framesBefore = p->frames;
    Parsed empty(fetched(b, p, fetchParams(kLoggerId, "${data}/log/logger.log", ",\"offset\":" + num((long long)log.size()) + ",\"compress\":\"deflate\""), &bytes));
    CHECK(empty.result.isObject() && integer(empty.result, "size") == 0 && !empty.result.find("stream").present() && p->frames == framesBefore);
    CHECK(Parsed(b.call(p, "files.fetch", fetchParams(kLoggerId, "${data}/log/logger.log", ",\"offset\":" + num((long long)log.size() + 1) + ",\"compress\":\"none\""))).code() == "out-of-range");
    // Only what files.list gives: not a file no entry names, not another way of writing a listed one, not the link,
    // and nothing of a package that is not installed.
    const char* unlisted[] = {"${data}/log/notes.txt", "${data}/log/../log/older.log", "${data}/log/keys.log", "${data}/log/", "/tmp/nbtapp/agent.log"};
    for (size_t i = 0; i < sizeof unlisted / sizeof unlisted[0]; ++i) {
        CHECK_IN(unlisted[i], Parsed(b.call(p, "files.fetch", fetchParams(kLoggerId, unlisted[i]))).code() == "not-found");
    }
    CHECK(Parsed(b.call(p, "files.fetch", fetchParams("org.nbtapp.runtime", "${data}/log/logger.log"))).code() == "not-found");

    // The agent's own logs, by their bare names.
    {
        std::string own = b.root + "/run/log/agent.log";
        b.agent.setOwnLog(own);
        put(own, "the agent's log\n");
        put(own + ".1", "the one before\n");
        touch(own, 1794400500);
        touch(own + ".1", 1794400400);
        Parsed r(b.call(p, "files.list", listParams(kAgentId)));
        json::Value files = r.result.find("files");
        CHECK(files.size() == 2 && str(files.at(0), "path") == "agent.log" && str(files.at(1), "path") == "agent.log.1");
        fetched(b, p, fetchParams(kAgentId, "agent.log.1"), &bytes);
        CHECK(bytes == "the one before\n");
        CHECK(Parsed(b.call(p, "files.fetch", fetchParams(kAgentId, "trial.log"))).code() == "not-found");
    }

    // A file that does not compress, of 2 MB, for the streams that follow.
    std::string big;
    unsigned state = 12345;
    for (int i = 0; i < 2000000; ++i) {
        state = state * 1103515245u + 12345u;
        big += (char)(state >> 16);
    }
    put(data + "/log/big.log", big);

    // On a slow link: frames of 4,096 bytes at most, and the link's other messages get through while the stream
    // runs. The phone reads nothing for a while, so the agent has to stop sending: its answer to ping and to a
    // second files.fetch are behind two frames at most, not behind the rest of the file.
    {
        Phone* slow = b.phone("2", 4096, "serial");
        slow->deaf = true;
        int64_t fetch = slow->request("files.fetch", fetchParams(kLoggerId, "${data}/log/big.log", ",\"compress\":\"none\""));
        turns(b, slow, 50);
        int64_t ping = slow->request("ping", "{}");
        int64_t second = slow->request("files.fetch", fetchParams(kLoggerId, "${data}/log/older.log"));
        turns(b, slow, 50);
        slow->deaf = false;
        b.settle();
        Parsed r(slow->reply(fetch));
        unsigned stream = (unsigned)integer(r.result, "stream");
        CHECK(Parsed(slow->reply(ping)).result.isObject() && Parsed(slow->reply(second)).code() == "busy");
        CHECK(slow->received[stream] == big && slow->frames >= big.size() / 4096);
        long pong = position(slow, "R" + num(ping)), end = position(slow, "D" + num(stream) + " last");
        CHECK_IN(num(pong) + " " + num(end), pong >= 0 && end > pong + 100);
    }

    // files.cancel: answered after the frames the agent had sent, and nothing of the stream after that answer.
    {
        p->deaf = true;
        int64_t fetch = p->request("files.fetch", fetchParams(kLoggerId, "${data}/log/big.log", ",\"compress\":\"none\""));
        turns(b, p, 20);
        p->deaf = false;
        p->pump();
        p->deaf = true;
        unsigned stream = (unsigned)integer(Parsed(p->reply(fetch)).result, "stream");
        int64_t cancel = p->request("files.cancel", "{\"stream\":" + num(stream) + "}");
        turns(b, p, 20);
        p->deaf = false;
        b.settle();
        CHECK(Parsed(p->reply(cancel)).result.isObject() && p->ended.count(stream) == 0);
        long answered = position(p, "R" + num(cancel));
        for (size_t i = 0; i < p->heard.size(); ++i) CHECK_IN(p->heard[i], p->heard[i] != "D" + num(stream) || (long)i < answered);
        CHECK(!p->received[stream].empty() && p->received[stream].size() < big.size() && big.compare(0, p->received[stream].size(), p->received[stream]) == 0);
        // The link takes another stream at once; a stream that has ended is cancelled with no more ado.
        fetched(b, p, fetchParams(kLoggerId, "${data}/log/older.log"), &bytes);
        CHECK(bytes == "an older log\n");
        CHECK(Parsed(b.call(p, "files.cancel", "{\"stream\":" + num(stream) + "}")).result.isObject());
        CHECK(Parsed(b.call(p, "files.cancel", "{\"stream\":0}")).code() == "bad-request");
    }

    // A file cut short while it is sent: files.error, and no last frame.
    {
        p->deaf = true;
        int64_t fetch = p->request("files.fetch", fetchParams(kLoggerId, "${data}/log/big.log", ",\"compress\":\"deflate\""));
        turns(b, p, 20);
        CHECK(truncate((data + "/log/big.log").c_str(), 1000) == 0);
        p->deaf = false;
        b.settle();
        unsigned stream = (unsigned)integer(Parsed(p->reply(fetch)).result, "stream");
        Parsed e(lastEvent(p, "files.error"));
        CHECK(integer(e.params, "stream") == stream && str(e.params, "code") == "io-error" && p->ended.count(stream) == 0);
        put(data + "/log/big.log", big);
    }

    // A link that ends in the middle of a stream ends the stream, and nothing else.
    {
        size_t links = b.agent.sessions();
        Phone* gone = b.phone("3");
        gone->deaf = true;
        gone->request("files.fetch", fetchParams(kLoggerId, "${data}/log/big.log", ",\"compress\":\"none\""));
        turns(b, gone, 20);
        gone->hangUp();
        b.settle();
        CHECK(b.agent.sessions() == links);
        fetched(b, p, fetchParams(kLoggerId, "${data}/log/older.log"), &bytes);
        CHECK(bytes == "an older log\n");
    }

    // While a job runs no stream starts, and the list is still given.
    {
        Phone* q = b.phone("4");
        std::string planId = planOk(b, q, kRuntime, "", kRuntime);
        q->serve = false;
        startJob(b, q, planId);
        CHECK(Parsed(b.call(p, "files.fetch", fetchParams(kLoggerId, "${data}/log/older.log"))).code() == "busy");
        CHECK(Parsed(b.call(p, "files.list", listParams(kLoggerId))).result.isObject());
        q->serve = true;
        q->answerHeld();
        b.settle();
        CHECK(has(outcome(q), "succeeded"));
    }

    // Not before auth on a phone's own link.
    {
        Phone* stranger = b.greeted("5", 4096, "serial");
        CHECK(Parsed(b.call(stranger, "files.list", listParams(kLoggerId))).code() == "unauthenticated");
    }

    // ${tmp} goes with the package; and after a power down, which empties it, the agent makes it again at its start.
    {
        std::string planId = planOk(b, p, "", kLoggerId, "");
        p->events.clear();
        startJob(b, p, planId);
        CHECK(has(outcome(p), "succeeded") && fs::kindOf(tmp) == fs::kNone && fs::kindOf(data) == fs::kDir);
        install(b, p, kLogger, kLogger);
        std::string why;
        CHECK_IN(why, fs::removeTree(b.root + "/tmp", &why) && fs::removeTree(b.root + "/run", &why));
        Bench again("files", true);
        CHECK(fs::isDir(tmp));
    }
}

static void testSession(const std::string& path) {
    std::string now = sessionText(), before = testutil::readFile(path);
    if (now != before) {
        fprintf(stderr, "%s is not what the agent says now. If the protocol changed on purpose, run\n  test_agent --write-session %s\n", path.c_str(), path.c_str());
        exit(1);
    }
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    bool write = argc > 2 && strcmp(argv[1], "--write-session") == 0;
    int base = write ? 3 : 1;
    gData = argc > base ? argv[base] : "build/testdata";
    std::string work = argc > base + 1 ? argv[base + 1] : "build/agent-host/work-agent";
    std::string session = write ? argv[2] : (argc > 3 ? argv[3] : "spec/protocol/session.vectors");
    char cwd[4096];
    CHECK(getcwd(cwd, sizeof cwd) != 0);
    gWork = work[0] == '/' ? work : std::string(cwd) + "/" + work;
    std::string why;
    CHECK_IN(why, fs::removeTree(gWork, &why) && fs::makeDirs(gWork, &why));
    // A test that ends a process to stand in for a crash loses nothing by not syncing, and syncing takes minutes.
    fs::setSyncing(false);
    loadPackages();

    if (write) {
        std::string text = sessionText();
        FILE* f = fopen(session.c_str(), "wb");
        CHECK_IN(session, f != 0);
        CHECK(fwrite(text.data(), 1, text.size(), f) == text.size());
        fclose(f);
        printf("wrote %s\n", session.c_str());
        return 0;
    }
    testHello();
    testLinkEnds();
    testSerialLink();
    testAuth();
    testPairing();
    testPhones();
    testLinkClosing();
    testInfo();
    testPlan();
    testJob();
    testJobFails();
    testHookOutput();
    testLog();
    testPauseAndResume();
    testCancel();
    testFetchFaults();
    testDuringStep();
    testSystemRestart();
    testAgentRestart();
    testPowerCut();
    testQuietStart();
    testCrashDuringJob();
    testAgentUpdate();
    testDetachedSignature();
    testAutostart();
    testFiles();
    testSession(session);
    fs::removeTree(gWork, 0);
    printf("test_agent: ok\n");
    return 0;
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for channels (agent/channels.cpp, and their part of agent/agent.cpp): channel.open and each of its
// refusals, the messages both ways, the closes from either side, a lost link, and the numbers. An Agent runs in this
// process on a HostPlatform folder with the test package chan installed, and phones talk to it over socket pairs,
// with frames of their own making. The program's end of a channel is a stand-in here, which the test drives.
//
// One thread: a phone writes, the agent takes a turn, the phone reads, until nothing moves any more. Time is a clock
// the test sets.
//
// usage: test_channels TESTDATA WORK

#include "../../agent/agent.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include <map>
#include <string>
#include <vector>

#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "../../agent/json.hpp"
#include "../../agent/programs.hpp"
#include "test_util.hpp"

using namespace nbt;

static std::string gData, gWork;
static std::string gPrograms;   // the folder of this program, where chan_echo and chan_feed are too
static TrustedKey gKey;

// Turns of the agent, a few milliseconds each, until `condition` holds, for what a program does in its own time. Fails
// after ten seconds.
#define WAIT_FOR(bench, condition)                                       \
    do {                                                                 \
        bool met_ = false;                                               \
        for (int wait_ = 0; wait_ < 2000 && !(met_ = (condition)); ++wait_) (bench).step(); \
        CHECK_IN(#condition, met_);                                      \
    } while (0)

static std::string num(long long value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    return text;
}

static bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---- a phone ------------------------------------------------------------------------------------------------------

struct Data {
    unsigned stream;
    bool last;
    std::string bytes;
};

class Phone {
public:
    explicit Phone(int fd) : maxFrame(65536), closed(false), holdFetches(false), fd_(fd), nextId_(1), nextBlob_(1) {
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        out_ = "NBTL";
    }
    ~Phone() { hangUp(); }
    void hangUp() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        closed = true;
    }

    void frame(unsigned type, unsigned flags, unsigned stream, const std::string& payload) {
        std::string f;
        f += (char)type;
        f += (char)flags;
        f += (char)(stream & 0xFF);
        f += (char)(stream >> 8);
        for (int i = 0; i < 4; ++i) f += (char)((payload.size() >> (8 * i)) & 0xFF);
        f += payload;
        unsigned long crc = ::crc32(0L, (const Bytef*)f.data(), (uInt)f.size());
        for (int i = 0; i < 4; ++i) f += (char)((crc >> (8 * i)) & 0xFF);
        out_ += f;
    }
    int64_t request(const std::string& method, const std::string& params) {
        int64_t id = nextId_++;
        frame(1, 0, 0, "{\"id\":" + num(id) + ",\"method\":\"" + method + "\",\"params\":" + params + "}");
        return id;
    }
    void event(const std::string& name, const std::string& params) {
        frame(1, 0, 0, "{\"event\":\"" + name + "\",\"params\":" + params + "}");
    }
    // A channel's message: one DATA frame with the last flag.
    void send(unsigned stream, const std::string& bytes) { frame(2, 2, stream, bytes); }

    bool pump() {
        if (fd_ < 0) return false;
        bool moved = false;
        while (!out_.empty()) {
            ssize_t n = write(fd_, out_.data(), out_.size());
            if (n <= 0) break;
            out_.erase(0, (size_t)n);
            moved = true;
        }
        char buffer[65536];
        for (;;) {
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
            CHECK(length <= maxFrame);
            if (in_.size() < 12 + length) break;
            std::string payload = in_.substr(8, length);
            if (p[0] == 2) {
                Data d;
                d.stream = p[2] | (p[3] << 8);
                d.last = (p[1] & 2) != 0;
                d.bytes = payload;
                data.push_back(d);
                heard.push_back("D" + num(d.stream));
            } else {
                handle(payload);
            }
            in_.erase(0, 12 + length);
        }
        return moved;
    }

    std::string reply(int64_t id) {
        CHECK_IN("no reply to request " + num(id), replies.count(id) != 0);
        std::string text = replies[id];
        replies.erase(id);
        return text;
    }
    // The DATA of a stream that came, in order, and taken out.
    std::vector<std::string> take(unsigned stream) {
        std::vector<std::string> out;
        for (size_t i = 0; i < data.size();) {
            if (data[i].stream != stream) {
                ++i;
                continue;
            }
            CHECK_IN("a channel's DATA frame with no last flag", data[i].last);
            out.push_back(data[i].bytes);
            data.erase(data.begin() + (long)i);
        }
        return out;
    }
    // The events of a name that came, and taken out.
    std::vector<std::string> events(const std::string& name) {
        std::vector<std::string> out;
        for (size_t i = 0; i < events_.size();) {
            if (!has(events_[i], "\"event\":\"" + name + "\"")) {
                ++i;
                continue;
            }
            out.push_back(events_[i]);
            events_.erase(events_.begin() + (long)i);
        }
        return out;
    }

    uint32_t maxFrame;
    bool closed;
    std::string hello;                // the answer to its hello
    std::map<int64_t, std::string> replies;
    std::vector<Data> data;
    std::vector<std::string> heard;   // "D<stream>", "R<id>", "E<event>", in order

private:
public:
    // A blob.fetch, answered from `blobs`: the reply, then the range in DATA frames on a stream of its own.
    void serve(const std::string& request) {
        json::Document doc;
        CHECK(doc.parse(request, 0));
        json::Value root = doc.root(), params = root.find("params");
        std::string sha = params.find("sha256").string();
        CHECK_IN(sha, blobs.count(sha) != 0);
        const std::string& blob = blobs[sha];
        size_t offset = (size_t)params.find("offset").integer();
        size_t length = params.find("length").isInteger() ? (size_t)params.find("length").integer() : blob.size() - offset;
        unsigned stream = nextBlob_++;
        frame(1, 0, 0, "{\"id\":" + num(root.find("id").integer()) + ",\"result\":{\"stream\":" + num(stream) + ",\"size\":" +
                           num((long long)length) + "}}");
        for (size_t at = 0; at < length; at += 4096) {
            size_t n = length - at < 4096 ? length - at : 4096;
            frame(2, at + n == length ? 2 : 0, stream, blob.substr(offset + at, n));
        }
    }
    std::map<std::string, std::string> blobs;   // what it serves, by hash
    bool holdFetches;                           // keeps blob.fetch requests in `held` instead of answering
    std::vector<std::string> held;

private:
    void handle(const std::string& text) {
        json::Document doc;
        CHECK_IN(text, doc.parse(text, 0));
        json::Value root = doc.root();
        if (root.find("method").isString() && root.find("method").string() == "blob.fetch") {
            if (holdFetches) held.push_back(text);
            else serve(text);
        } else if (root.find("method").isString()) {
            CHECK_IN(text, root.find("method").string() == "ping");
            frame(1, 0, 0, "{\"id\":" + num(root.find("id").integer()) + ",\"result\":{}}");
        } else if (root.find("event").isString()) {
            events_.push_back(text);
            heard.push_back("E" + root.find("event").string());
        } else {
            replies[root.find("id").integer()] = text;
            heard.push_back("R" + num(root.find("id").integer()));
        }
    }

    int fd_;
    int64_t nextId_;
    unsigned nextBlob_;
    std::string out_, in_;
    std::vector<std::string> events_;
};

// ---- the test packages, as a phone offers them --------------------------------------------------------------------

struct TestOffer {
    std::string id, version, text;     // text: the offer's JSON
    std::string sha256, manifestSha256, bytes, manifest;
};

// "org.nbtapp.radio-1.0.0": from vectors.json and the package's file.
static TestOffer offerOf(const std::string& name) {
    std::string text = testutil::readFile(gData + "/vectors.json");
    json::Document doc;
    CHECK(doc.parse(text, 0));
    json::Value good = doc.root().find("good");
    for (size_t i = 0; i < good.size(); ++i) {
        json::Value g = good.at(i);
        if (g.find("file").string() != "good/" + name + ".nbtpkg") continue;
        TestOffer o;
        o.id = g.find("id").string();
        o.version = g.find("version").string();
        o.sha256 = g.find("sha256").string();
        o.manifestSha256 = g.find("manifestSha256").string();
        o.bytes = testutil::readFile(gData + "/" + g.find("file").string());
        MemorySource source(o.bytes.data(), o.bytes.size());
        Package package;
        Error error;
        CHECK_IN(name + ": " + error.message, package.open(&source, std::vector<TrustedKey>(1, gKey), &error));
        o.manifest = package.manifestBytes();
        o.text = "{\"id\":\"" + o.id + "\",\"version\":\"" + o.version + "\",\"sha256\":\"" + o.sha256 + "\",\"size\":" +
                 num((long long)o.bytes.size()) + ",\"manifestSha256\":\"" + o.manifestSha256 + "\",\"signature\":\"" +
                 g.find("signature").string() + "\"}";
        return o;
    }
    CHECK_IN(name, false);
    return TestOffer();
}

// ---- a stand-in for the program -----------------------------------------------------------------------------------

// What one end of a channel saw, kept after the end is gone.
struct EndRecord {
    std::string path;
    bool alive;            // the agent has not closed its end
    std::string got;       // the bytes the agent wrote
    std::string toAgent;   // what the program is to send, taken as the agent reads
    bool closing;          // the program closes its end once toAgent is read
    size_t takes;          // the most bytes it takes from one write; 0 for none at all
    EndRecord() : alive(true), closing(false), takes((size_t)-1) {}

    // The first line, then the messages the agent wrote, as the program would read them.
    std::string line() const { return got.substr(0, got.find('\n')); }
    std::vector<std::string> messages() const {
        std::vector<std::string> out;
        size_t at = got.find('\n') + 1;
        while (at + 2 <= got.size()) {
            size_t length = ((size_t)(unsigned char)got[at] << 8) | (unsigned char)got[at + 1];
            out.push_back(got.substr(at + 2, length));
            at += 2 + length;
        }
        return out;
    }
};

class StandInEnd : public ProgramEnd {
public:
    explicit StandInEnd(EndRecord* r) : r_(r) {}
    virtual ~StandInEnd() { r_->alive = false; }
    virtual Connect connect(std::string*) { return kConnected; }
    virtual int fd() const { return -1; }
    virtual bool read(std::string* in, size_t most) {
        size_t n = r_->toAgent.size() < most ? r_->toAgent.size() : most;
        in->append(r_->toAgent, 0, n);
        r_->toAgent.erase(0, n);
        return !(r_->closing && r_->toAgent.empty());
    }
    virtual long write(const char* bytes, size_t len) {
        if (r_->closing) return -1;
        size_t n = len < r_->takes ? len : r_->takes;
        r_->got.append(bytes, n);
        return (long)n;
    }

private:
    EndRecord* r_;
};

class StandIns : public ProgramEnds {
public:
    StandIns() : refuse(false), wait(false) {}
    ~StandIns() {
        for (size_t i = 0; i < records.size(); ++i) delete records[i];
    }
    virtual ProgramEnd* make(const std::string& path) {
        EndRecord* r = new EndRecord();
        r->path = path;
        records.push_back(r);
        if (refuse) return new Refusing();
        if (wait) return new Waiting();
        return new StandInEnd(r);
    }
    EndRecord* last() { return records.empty() ? 0 : records.back(); }

    bool refuse;   // the next ends find nothing listening
    bool wait;     // the next ends' connections are never taken, nor refused
    std::vector<EndRecord*> records;

private:
    class Waiting : public ProgramEnd {
    public:
        virtual Connect connect(std::string*) { return kWaiting; }
        virtual int fd() const { return -1; }
        virtual bool read(std::string*, size_t) { return true; }
        virtual long write(const char*, size_t) { return 0; }
    };
    class Refusing : public ProgramEnd {
    public:
        virtual Connect connect(std::string* why) {
            *why = "nothing listens";
            return kRefused;
        }
        virtual int fd() const { return -1; }
        virtual bool read(std::string*, size_t) { return false; }
        virtual long write(const char*, size_t) { return -1; }
    };
};

// ---- a head unit in a folder, with the test package chan installed ------------------------------------------------

class FakeClock : public Clock {
public:
    FakeClock() : now(1000) {}
    virtual int64_t nowMs() { return now; }
    int64_t now;
};

class FakeRandom : public RandomSource {
public:
    FakeRandom() : next(0) {}
    virtual void bytes(uint8_t* out, size_t len) {
        for (size_t i = 0; i < len; ++i) out[i] = next++;
    }
    virtual bool secret(uint8_t* out, size_t len) {
        bytes(out, len);
        return true;
    }
    uint8_t next;
};

static std::vector<TrustedKey> keys() { return std::vector<TrustedKey>(1, gKey); }

// A local socket's path has a limit of about 100 bytes, which a work folder deep in a checkout passes: the ${tmp} of
// a test's head unit, where the programs' sockets are, is a short folder of its own under /tmp. The rest stays in
// the work folder, as in the bench's container nothing under /tmp may be run.
static std::string shortFolder(const std::string& name) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/nbtch-%s-XXXXXX", name.c_str());
    CHECK_IN(name, mkdtemp(path) != 0);
    return path;
}

// A folder that stands in for a head unit, with ${tmp} in a short folder, where a test may forbid the agent to start
// programs.
class TestPlatform : public HostPlatform {
public:
    TestPlatform(const std::string& root, const std::string& tmp) : HostPlatform(root), starts(true), tmp_(tmp) {}
    virtual bool canStartPrograms() const { return starts; }
    virtual std::string tmpDir(const std::string& id) const { return tmp_ + "/" + id; }
    bool starts;

private:
    std::string tmp_;
};

static std::string emptied(const std::string& folder) {
    std::string why;
    CHECK_IN(why, fs::removeTree(folder, &why));
    return folder;
}

struct Bench {
    std::string root, tmp;
    TestPlatform platform;
    FakeClock clock;
    FakeRandom random;
    StandIns ends;
    Agent* agent;
    bool sockets;
    std::vector<Phone*> phones;
    std::vector<pid_t> programs;   // the ones the test started
    std::vector<pid_t> orphans;    // the ones an agent before restart() started

    // With `sockets`, the programs' ends are their sockets, as on a head unit; otherwise stand-ins. With `trial`, the
    // agent is a version on trial, as the launcher starts it after its own update.
    explicit Bench(const std::string& name, bool sockets = false, bool trial = false)
        : root(emptied(gWork + "/" + name)), tmp(shortFolder(name)), platform(root, tmp), agent(0), sockets(sockets) {
        std::string why;
        CHECK_IN(why, platform.prepare(&why));
        if (trial) {
            CHECK_IN(why, fs::makeDirs(platform.agentHome(), &why) && fs::writeFile(platform.agentHome() + "/current", "1.6.0\n", 0644, &why) &&
                              fs::writeFile(platform.agentHome() + "/trial", "1.7.0\n", 0644, &why));
        }
        start();
        CHECK(agent->onTrial() == trial);
        StepReport r = agent->engine().install(gData + "/good/org.nbtapp.chan-1.0.0.nbtpkg", "", 0);
        CHECK_IN(r.error.code + ": " + r.error.message, r.outcome == kStepDone);
    }
    ~Bench() {
        stop();
        for (size_t i = 0; i < programs.size(); ++i) {
            kill(programs[i], SIGKILL);
            waitpid(programs[i], 0, 0);
        }
        // What the agent started, by its marks: each in a process group of its own, and a child of this process.
        std::vector<std::string> names;
        fs::listDir(platform.runDir(), &names, 0);
        for (size_t i = 0; i < names.size(); ++i) {
            std::string text;
            if (names[i].compare(0, 12, "channel-pid.") != 0 || !fs::readFile(platform.runDir() + "/" + names[i], 32, &text, 0)) continue;
            pid_t pid = (pid_t)atol(text.c_str());
            if (pid <= 1) continue;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, 0, 0);
        }
        fs::removeTree(root, 0);
        fs::removeTree(tmp, 0);
    }

    void start() {
        std::string why;
        agent = new Agent(&platform, keys(), "1.7.0", &clock, &random);
        agent->setLog(0);
        CHECK_IN(why, agent->init(&why));
        if (!sockets) agent->setProgramEnds(&ends);
    }
    void stop() {
        for (size_t i = 0; i < phones.size(); ++i) delete phones[i];
        phones.clear();
        delete agent;
        agent = 0;
    }
    // The agent ends, and another starts on the same head unit: the run folder is kept, as it is when no power down
    // came between. What the first started is no child of the second's: this process collects it, as the system
    // collects the children of a process that has ended.
    void restart() {
        std::vector<std::string> names;
        fs::listDir(platform.runDir(), &names, 0);
        for (size_t i = 0; i < names.size(); ++i) {
            std::string text;
            if (names[i].compare(0, 12, "channel-pid.") == 0 && fs::readFile(platform.runDir() + "/" + names[i], 32, &text, 0)) {
                orphans.push_back((pid_t)atol(text.c_str()));
            }
        }
        stop();
        start();
    }

    // The path a channel's program listens at.
    std::string socketOf(const std::string& name) const { return platform.tmpDir("org.nbtapp.chan") + "/channels/" + name; }

    // A test program of the bench (chan_echo, chan_feed) listening at a channel's socket, started by the test as the
    // agent would start it; returns once it listens.
    pid_t run(const std::string& program, const std::string& channel, const char* a = 0, const char* b = 0) {
        std::string path = gPrograms + "/" + program, socket = socketOf(channel);
        unlink(socket.c_str());   // so that the socket that appears is the program's
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            int null = ::open("/dev/null", O_WRONLY);
            dup2(null, 2);
            execl(path.c_str(), program.c_str(), "--socket", socket.c_str(), a, b, (char*)0);
            _exit(127);
        }
        programs.push_back(pid);
        for (int i = 0; i < 500 && fs::kindOf(socket) == fs::kNone; ++i) usleep(10000);
        CHECK_IN(socket, fs::kindOf(socket) != fs::kNone);
        return pid;
    }

    // One turn that waits a little for what a program sends. The clock goes on by as much, and the programs the test
    // started that have ended are collected, as their parent would.
    void step() {
        for (size_t i = 0; i < phones.size(); ++i) phones[i]->pump();
        agent->turn(5);
        clock.now += 5;
        for (size_t i = 0; i < phones.size(); ++i) phones[i]->pump();
        for (size_t i = 0; i < programs.size(); ++i) waitpid(programs[i], 0, WNOHANG);
        for (size_t i = 0; i < orphans.size(); ++i) waitpid(orphans[i], 0, WNOHANG);
    }

    Phone* connect(const char* kind) {
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        agent->adopt(new SocketLink(fds[0], kind));
        Phone* p = new Phone(fds[1]);
        phones.push_back(p);
        return p;
    }

    // A controller of `role` on the local socket, with hello done.
    Phone* controller(const char* role = "cli", unsigned maxFrame = 65536) {
        Phone* p = connect("local");
        p->maxFrame = maxFrame;
        std::string extra = strcmp(role, "phone") == 0 ? ",\"phoneId\":\"" + std::string(32, '1') + "\",\"name\":\"Test phone\"" : "";
        std::string text = call(p, "hello", std::string("{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"") + role + "\"" + extra +
                                                ",\"maxFrame\":" + num(maxFrame) + ",\"capabilities\":[]}");
        CHECK_IN(text, has(text, "\"result\""));
        p->hello = text;
        return p;
    }

    void settle() {
        int quiet = 0;
        for (int round = 0; round < 200000 && quiet < 3; ++round) {
            bool moved = false;
            for (size_t i = 0; i < phones.size(); ++i) moved = phones[i]->pump() || moved;
            agent->turn(0);
            for (size_t i = 0; i < phones.size(); ++i) moved = phones[i]->pump() || moved;
            quiet = moved ? 0 : quiet + 1;
        }
        CHECK(quiet >= 3);
    }

    std::string call(Phone* p, const std::string& method, const std::string& params) {
        int64_t id = p->request(method, params);
        settle();
        return p->reply(id);
    }

    // job.plan of an install from offers the phone serves; returns the plan's id.
    std::string plan(Phone* p, const TestOffer& o) {
        p->blobs[o.sha256] = o.bytes;
        p->blobs[o.manifestSha256] = o.manifest;
        std::string text = call(p, "job.plan", "{\"install\":[{\"id\":\"" + o.id + "\",\"version\":\"" + o.version +
                                                   "\"}],\"remove\":[],\"offers\":[" + o.text + "]}");
        json::Document doc;
        CHECK_IN(text, doc.parse(text, 0) && doc.root().find("result").find("planId").isString());
        return doc.root().find("result").find("planId").string();
    }

    // channel.open of a channel of chan; the answer as it came.
    std::string open(Phone* p, const std::string& name, const std::string& more = "") {
        return call(p, "channel.open", "{\"package\":\"org.nbtapp.chan\",\"channel\":\"" + name + "\"" + more + "}");
    }
};

static std::string code(const std::string& text) {
    json::Document doc;
    CHECK_IN(text, doc.parse(text, 0));
    json::Value error = doc.root().find("error");
    return error.isObject() ? error.find("code").string() : std::string();
}

static long long member(const std::string& text, const char* name) {
    json::Document doc;
    CHECK_IN(text, doc.parse(text, 0));
    json::Value v = doc.root().find("result").find(name);
    return v.isInteger() ? (long long)v.integer() : -1;
}

// The messages of a stream that have come, added to *into: how many there are now.
static size_t collect(Phone* p, unsigned stream, std::vector<std::string>* into) {
    std::vector<std::string> more = p->take(stream);
    into->insert(into->end(), more.begin(), more.end());
    return into->size();
}

// The stream of an open that was answered.
static unsigned openedOn(const std::string& text) {
    long long stream = member(text, "stream");
    CHECK_IN(text, stream >= kChannelFirstStream && stream <= 65535);
    return (unsigned)stream;
}

// ---- the tests ----------------------------------------------------------------------------------------------------

// hello lists the capability; an open is answered with the stream, the sizes and the package's version; the program
// is told who asks; messages go both ways; a close from the phone and from the program.
static void testOpen() {
    Bench b("open");
    Phone* p = b.controller();
    CHECK_IN(p->hello, has(p->hello, "\"settings\",\"channels\""));

    std::string text = b.open(p, "echo");
    CHECK_IN(text, openedOn(text) == kChannelFirstStream && member(text, "maxMessage") == (long long)kChannelMaxMessage);
    CHECK_IN(text, member(text, "credit") == (long long)kChannelCredit && has(text, "\"version\":\"1.0.0\""));
    EndRecord* r = b.ends.last();
    CHECK_IN(r->path, r->path == b.platform.tmpDir("org.nbtapp.chan") + "/channels/echo");
    CHECK_IN(r->line(), r->line() == "{\"v\":1,\"channel\":\"echo\",\"access\":\"both\",\"caller\":\"cli\",\"maxMessage\":4096}");
    CHECK(b.agent->channels().count("org.nbtapp.chan", "echo") == 1);

    // Both ways, whole messages, of 1 and of 4,096 bytes.
    p->send(kChannelFirstStream, "a");
    p->send(kChannelFirstStream, std::string(4096, 'b'));
    b.settle();
    CHECK(r->messages().size() == 2 && r->messages()[0] == "a" && r->messages()[1] == std::string(4096, 'b'));
    r->toAgent = channelFrame("one") + channelFrame(std::string(4096, 'z'));
    b.settle();
    std::vector<std::string> got = p->take(kChannelFirstStream);
    CHECK(got.size() == 2 && got[0] == "one" && got[1] == std::string(4096, 'z'));

    // The phone closes it: the program's end goes, and nothing is said back.
    p->event("channel.close", "{\"stream\":" + num(kChannelFirstStream) + ",\"reason\":\"closed\"}");
    b.settle();
    CHECK(!r->alive && b.agent->channels().count("org.nbtapp.chan", "echo") == 0 && p->events("channel.close").empty());

    // The program closes it, after what it had to say: the phone gets that, then the reason.
    text = b.open(p, "echo");
    unsigned stream = openedOn(text);
    CHECK_IN(text, stream == kChannelFirstStream + 1);
    r = b.ends.last();
    r->toAgent = channelFrame("last words");
    r->closing = true;
    b.settle();
    CHECK(p->take(stream).size() == 1);
    std::vector<std::string> closes = p->events("channel.close");
    CHECK(closes.size() == 1 && has(closes[0], "\"stream\":" + num(stream)) && has(closes[0], "\"reason\":\"closed\""));
    CHECK(!r->alive);

    // A phone, which the program is told by its name.
    Phone* phone = b.controller("phone");
    text = b.open(phone, "feed");
    CHECK_IN(text, openedOn(text) == kChannelFirstStream);   // the numbers are a link's own
    CHECK_IN(b.ends.last()->line(), has(b.ends.last()->line(), "\"access\":\"read\",\"caller\":\"nbtapps\",\"phone\":\"Test phone\""));
    printf("  open, messages both ways, closes from either side\n");
}

// Each refusal of channel.open, in the order the checks are made.
static void testRefusals() {
    Bench b("refusals");
    Phone* p = b.controller();
    CHECK(code(b.call(p, "channel.open", "{\"package\":\"org.nbtapp.chan\"}")) == "bad-request");
    CHECK(code(b.call(p, "channel.open", "{\"package\":\"chan\",\"channel\":\"echo\"}")) == "bad-request");
    CHECK(code(b.open(p, "echo", ",\"credit\":0")) == "bad-request");
    CHECK(code(b.open(p, "echo", ",\"credit\":65536")) == "bad-request");
    CHECK(code(b.call(p, "channel.open", "{\"package\":\"org.nbtapp.none\",\"channel\":\"echo\"}")) == "not-found");
    CHECK(code(b.open(p, "other")) == "not-found");

    Phone* plugin = b.controller("plugin");
    CHECK(code(b.open(plugin, "echo")) == "forbidden");
    // A phone app other than NBTApps.
    CHECK(code(b.open(p, "echo", ",\"app\":\"org.example.app\"")) == "unsupported");
    // A link whose frame does not hold a message.
    Phone* small = b.controller("cli", 1024);
    CHECK(code(b.open(small, "echo")) == "unsupported");

    // Nothing listens, and nothing is to be started: unavailable. The numbers count on.
    b.ends.refuse = true;
    b.platform.starts = false;
    std::string text = b.open(p, "echo");
    CHECK_IN(text, code(text) == "unavailable" && has(text, "nothing listens"));
    b.ends.refuse = false;
    CHECK(openedOn(b.open(p, "echo")) == kChannelFirstStream + 1);

    // Eight on a link, and not one more; another link has its own eight.
    for (unsigned i = 1; i < kChannelPerLink; ++i) CHECK(code(b.open(p, i % 2 == 0 ? "echo" : "feed")) == "");
    text = b.open(p, "echo");
    CHECK_IN(text, code(text) == "too-many");
    Phone* other = b.controller();
    CHECK(code(b.open(other, "echo")) == "");
    printf("  each refusal of channel.open\n");
}

// What breaks the rules of a channel closes that channel and leaves the link; a number that is not open is passed
// over; a lost link closes its channels.
static void testFaults() {
    Bench b("faults");
    Phone* p = b.controller();
    unsigned echo = openedOn(b.open(p, "echo")), feed = openedOn(b.open(p, "feed"));
    EndRecord* feedEnd = b.ends.last();

    // Not open: dropped, and the link goes on.
    p->send(kChannelFirstStream + 7, "nobody");
    p->event("channel.credit", "{\"stream\":" + num(kChannelFirstStream + 7) + ",\"messages\":1}");
    p->event("channel.close", "{\"stream\":" + num(kChannelFirstStream + 7) + "}");
    CHECK(code(b.call(p, "ping", "{}")) == "" && !p->closed);

    // A message on a channel the phone may only read.
    p->send(feed, "x");
    b.settle();
    std::vector<std::string> closes = p->events("channel.close");
    CHECK(closes.size() == 1 && has(closes[0], "\"stream\":" + num(feed)) && has(closes[0], "\"reason\":\"protocol\""));
    CHECK(!feedEnd->alive);

    // No last flag, empty, too long: each closes its channel.
    const char* const faults[] = {"no last flag", "empty", "too long", 0};
    for (int i = 0; faults[i] != 0; ++i) {
        if (i > 0) echo = openedOn(b.open(p, "echo"));
        EndRecord* r = b.ends.last();
        if (i == 0) p->frame(2, 0, echo, "x");
        if (i == 1) p->send(echo, "");
        if (i == 2) p->send(echo, std::string(kChannelMaxMessage + 1, 'x'));
        b.settle();
        closes = p->events("channel.close");
        CHECK_IN(faults[i], closes.size() == 1 && has(closes[0], "\"reason\":\"protocol\"") && !r->alive && !p->closed);
        CHECK_IN(faults[i], r->messages().empty());
    }

    // A program that sends what is not a message.
    echo = openedOn(b.open(p, "echo"));
    EndRecord* r = b.ends.last();
    r->toAgent = std::string("\0\0", 2);
    b.settle();
    closes = p->events("channel.close");
    CHECK(closes.size() == 1 && has(closes[0], "\"reason\":\"protocol\"") && !r->alive);

    // The link goes: its channels close, and the programs' ends with them.
    echo = openedOn(b.open(p, "echo"));
    r = b.ends.last();
    p->hangUp();
    b.settle();
    CHECK(!r->alive && b.agent->channels().count("org.nbtapp.chan", "echo") == 0);
    printf("  faults close their channel, a lost link closes its channels\n");
}

// A files stream's numbers run to kStreamLast, and start again at 1: the numbers above are the channels'.
static void testStreamNumbers() {
    Bench b("numbers");
    Phone* p = b.controller();
    std::string log = b.root + "/agent.log";
    CHECK(fs::writeFile(log, "x\n", 0644, 0));
    b.agent->setOwnLog(log);
    unsigned last = 0;
    for (unsigned i = 0; i <= kStreamLast; ++i) {
        std::string text = b.call(p, "files.fetch", "{\"package\":\"org.nbtapp.agent\",\"area\":\"logs\",\"path\":\"agent.log\",\"compress\":\"none\"}");
        last = (unsigned)member(text, "stream");
        CHECK_IN(text, last == (i < kStreamLast ? i + 1 : 1));
        CHECK(p->take(last).size() == 1);
    }
    CHECK(last == 1);
    printf("  a files stream's numbers start again after %u\n", (unsigned)kStreamLast);
}

// The program's end on its socket, with chan_echo behind it: messages of 1 and of 4,096 bytes both ways, a program
// that closes, one that reads slowly, a socket nobody listens on, and none at all.
static void testSocket() {
    Bench b("socket", true);
    b.platform.starts = false;   // the test starts the programs here
    Phone* p = b.controller();
    // The folder is the agent's alone.
    struct stat st;
    std::string dir = channelsDir(b.platform.tmpDir("org.nbtapp.chan"));
    CHECK(stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    // It is on the RAM disk, which a power down empties: an agent that starts makes it again.
    std::string why;
    CHECK_IN(why, fs::removeTree(b.platform.tmpDir("org.nbtapp.chan"), &why));
    {
        Agent again(&b.platform, keys(), "1.7.0", &b.clock, &b.random);
        again.setLog(0);
        CHECK_IN(why, again.init(&why));
    }
    CHECK(stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);

    // No socket, then one nothing listens on (a program that died left it).
    std::string text = b.open(p, "echo");
    CHECK_IN(text, code(text) == "unavailable" && has(text, "there is no socket"));
    int stale = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, b.socketOf("echo").c_str());
    CHECK(bind(stale, (struct sockaddr*)&address, sizeof address) == 0);
    close(stale);
    text = b.open(p, "echo");
    CHECK_IN(text, code(text) == "unavailable" && has(text, "nothing listens"));

    b.run("chan_echo", "echo");
    unsigned echo = openedOn(b.open(p, "echo"));
    p->send(echo, "a");
    p->send(echo, std::string(kChannelMaxMessage, 'b'));
    std::vector<std::string> got;
    WAIT_FOR(b, collect(p, echo, &got) == 2);
    CHECK(got[0] == "a" && got[1] == std::string(kChannelMaxMessage, 'b'));
    // Two channels to one program, each on its own connection.
    unsigned second = openedOn(b.open(p, "echo"));
    p->send(second, "second");
    p->send(echo, "first");
    std::vector<std::string> one, two;
    WAIT_FOR(b, collect(p, echo, &one) + collect(p, second, &two) == 2);
    CHECK(one[0] == "first" && two[0] == "second");

    // A program that closes the connection after two messages: the two come back, then the reason.
    b.run("chan_echo", "gated", "--close-after", "2");
    CHECK(code(b.call(p, "channel.set", "{\"package\":\"org.nbtapp.chan\",\"channel\":\"gated\",\"on\":true}")) == "");
    unsigned closing = openedOn(b.open(p, "gated"));
    p->send(closing, "x");
    p->send(closing, "y");
    WAIT_FOR(b, !p->events("channel.close").empty());
    got = p->take(closing);
    CHECK(got.size() == 2 && got[0] == "x" && got[1] == "y");

    printf("  the program's socket: messages both ways, a program that closes, no program\n");
}

// A program that reads slowly: every message gets there, in order, and the link goes on meanwhile.
static void testSlowReader() {
    Bench b("slow", true);
    b.platform.starts = false;
    Phone* p = b.controller();
    b.run("chan_echo", "echo", "--slow", "20");
    unsigned echo = openedOn(b.open(p, "echo"));
    for (int i = 0; i < 10; ++i) p->send(echo, "m" + num(i));
    std::vector<std::string> got;
    WAIT_FOR(b, collect(p, echo, &got) == 10);
    for (int i = 0; i < 10; ++i) CHECK(got[(size_t)i] == "m" + num(i));
    CHECK(code(b.call(p, "ping", "{}")) == "");
    printf("  a program that reads slowly gets every message, in order\n");
}

// The sum of the `messages` of the channel.credit events of a stream that came, which are taken out.
static unsigned granted(Phone* p, unsigned stream) {
    unsigned sum = 0;
    std::vector<std::string> credits = p->events("channel.credit");
    for (size_t i = 0; i < credits.size(); ++i) {
        json::Document doc;
        CHECK(doc.parse(credits[i], 0));
        json::Value params = doc.root().find("params");
        CHECK_IN(credits[i], params.find("stream").integer() == (int64_t)stream && params.find("messages").integer() >= 1);
        sum += (unsigned)params.find("messages").integer();
    }
    return sum;
}

// Credits both ways: a phone that grants nothing stalls its own channel, and only that; a program that does not read
// holds the phone back; what the program takes is granted back, half a credit at a time; a message over the credit
// closes the channel.
static void testCredits() {
    Bench b("credits");
    Phone* p = b.controller();
    unsigned held = openedOn(b.open(p, "echo", ",\"credit\":1"));
    EndRecord* heldEnd = b.ends.last();
    unsigned free = openedOn(b.open(p, "echo"));
    EndRecord* freeEnd = b.ends.last();
    for (int i = 0; i < 5; ++i) heldEnd->toAgent += channelFrame("held " + num(i));
    for (int i = 0; i < 3; ++i) freeEnd->toAgent += channelFrame("free " + num(i));
    b.settle();
    CHECK(p->take(held).size() == 1 && p->take(free).size() == 3);
    CHECK(code(b.call(p, "ping", "{}")) == "");
    // The phone grants two more, and gets two more; the rest waits.
    p->event("channel.credit", "{\"stream\":" + num(held) + ",\"messages\":2}");
    b.settle();
    std::vector<std::string> got = p->take(held);
    CHECK(got.size() == 2 && got[0] == "held 1" && got[1] == "held 2");
    p->event("channel.credit", "{\"stream\":" + num(held) + ",\"messages\":100}");
    b.settle();
    CHECK(p->take(held).size() == 2);

    // A program that takes nothing: the phone's sixteen wait here, nothing is granted back, and a seventeenth closes
    // the channel.
    freeEnd->takes = 0;
    for (unsigned i = 0; i < kChannelCredit; ++i) p->send(free, "m" + num(i));
    b.settle();
    CHECK(granted(p, free) == 0 && freeEnd->messages().empty() && freeEnd->alive);
    // Then it takes them all: they are granted back, half a credit at a time.
    freeEnd->takes = (size_t)-1;
    b.settle();
    CHECK(freeEnd->messages().size() == kChannelCredit);
    std::vector<std::string> credits = p->events("channel.credit");
    CHECK(credits.size() == 2 && has(credits[0], "\"messages\":8") && has(credits[1], "\"messages\":8"));
    // Fewer than half: granted once the program has taken them all.
    for (int i = 0; i < 3; ++i) p->send(free, "n" + num(i));
    b.settle();
    CHECK(granted(p, free) == 3);
    // Over the credit: a seventeenth while the program takes nothing.
    freeEnd->takes = 0;
    for (unsigned i = 0; i <= kChannelCredit; ++i) p->send(free, "o" + num(i));
    b.settle();
    std::vector<std::string> closes = p->events("channel.close");
    CHECK(closes.size() == 1 && has(closes[0], "\"stream\":" + num(free)) && has(closes[0], "\"reason\":\"protocol\"") && !freeEnd->alive);
    // A credit that is not one closes its channel too.
    p->event("channel.credit", "{\"stream\":" + num(held) + ",\"messages\":0}");
    b.settle();
    closes = p->events("channel.close");
    CHECK(closes.size() == 1 && has(closes[0], "\"stream\":" + num(held)) && !heldEnd->alive);
    printf("  credits both ways\n");
}

// Turns: a channel that floods beside a quiet one and a files stream, each getting its turn on the link.
static void testTurns() {
    Bench b("turns");
    Phone* p = b.controller("cli", 4096);
    std::string log = b.root + "/agent.log";
    CHECK(fs::writeFile(log, std::string(40000, 'L'), 0644, 0));
    b.agent->setOwnLog(log);
    unsigned flood = openedOn(b.open(p, "echo", ",\"credit\":1000"));
    EndRecord* floodEnd = b.ends.last();
    unsigned quiet = openedOn(b.open(p, "echo"));
    EndRecord* quietEnd = b.ends.last();
    for (int i = 0; i < 200; ++i) floodEnd->toAgent += channelFrame(std::string(kChannelMaxMessage, 'f'));
    quietEnd->toAgent = channelFrame("quiet");
    int64_t id = p->request("files.fetch", "{\"package\":\"org.nbtapp.agent\",\"area\":\"logs\",\"path\":\"agent.log\",\"compress\":\"none\"}");
    b.settle();
    unsigned files = (unsigned)member(p->reply(id), "stream");
    // Where each came in the order the phone heard them: the quiet message and every frame of the file come long
    // before the flood is over.
    long lastFlood = -1, quietAt = -1, lastFile = -1;
    for (size_t i = 0; i < p->heard.size(); ++i) {
        if (p->heard[i] == "D" + num(flood)) lastFlood = (long)i;
        if (p->heard[i] == "D" + num(quiet) && quietAt < 0) quietAt = (long)i;
        if (p->heard[i] == "D" + num(files)) lastFile = (long)i;
    }
    CHECK(p->take(flood).size() == 200 && p->take(quiet).size() == 1);
    CHECK(lastFlood > 0 && quietAt >= 0 && lastFile >= 0);
    CHECK(quietAt < 10 && lastFile < 40 && lastFile < lastFlood);
    printf("  a flooding channel, a quiet one and a files stream take turns\n");
}

// Where an event or a reply came among what the phone heard, or -1.
static long heardAt(const Phone* p, const std::string& what) {
    for (size_t i = 0; i < p->heard.size(); ++i) {
        if (p->heard[i] == what) return (long)i;
    }
    return -1;
}

// A job across an open channel: paused at job.start, nothing moves and nothing opens while it runs, nothing is lost,
// and it goes on when the job has ended. A job that changes the package of an open channel closes that channel
// first. An agent on trial opens nothing.
static void testJobs() {
    {
        Bench b("job");
        Phone* p = b.controller();
        unsigned echo = openedOn(b.open(p, "echo"));
        EndRecord* r = b.ends.last();
        std::string planId = b.plan(p, offerOf("org.nbtapp.radio-1.0.0"));
        // The archive is held back: the job stays at its fetch.
        p->holdFetches = true;
        std::string text = b.call(p, "job.start", "{\"planId\":\"" + planId + "\"}");
        CHECK_IN(text, code(text) == "");
        CHECK(heardAt(p, "Echannel.paused") >= 0 && p->events("channel.paused").size() == 1);
        r->toAgent = channelFrame("from the program");
        p->send(echo, "to the program");
        b.settle();
        CHECK(p->take(echo).empty() && r->messages().empty());
        CHECK(code(b.open(p, "feed")) == "busy");
        // The fetch is answered: the job runs to its end, and the channel goes on.
        p->holdFetches = false;
        for (size_t i = 0; i < p->held.size(); ++i) p->serve(p->held[i]);
        p->held.clear();
        b.settle();
        CHECK(b.agent->engine().store().isInstalled("org.nbtapp.radio"));
        long done = heardAt(p, "Ejob.done"), resumed = heardAt(p, "Echannel.resumed");
        CHECK(done >= 0 && resumed > done && p->events("channel.resumed").size() == 1);
        std::vector<std::string> got = p->take(echo);
        CHECK(got.size() == 1 && got[0] == "from the program" && r->messages().size() == 1 && r->alive);
        CHECK(code(b.open(p, "feed")) == "");
    }
    {
        // An upgrade of chan itself: its channels close before the step, and open again after it.
        Bench b("upgrade");
        Phone* p = b.controller();
        Phone* other = b.controller();
        unsigned echo = openedOn(b.open(p, "echo"));
        EndRecord* r = b.ends.last();
        std::string planId = b.plan(other, offerOf("org.nbtapp.chan-1.1.0"));
        CHECK(code(b.call(other, "job.start", "{\"planId\":\"" + planId + "\"}")) == "");
        b.settle();
        std::vector<std::string> closes = p->events("channel.close");
        CHECK(closes.size() == 1 && has(closes[0], "\"stream\":" + num(echo)) && has(closes[0], "\"reason\":\"package-changed\""));
        CHECK(!r->alive && p->events("channel.paused").size() == 1);
        // The link was told it is paused, so it is told it goes on, though it has no channel now.
        CHECK(p->events("channel.resumed").size() == 1 && other->events("channel.paused").empty());
        std::string text = b.open(p, "echo");
        CHECK_IN(text, has(text, "\"version\":\"1.1.0\""));
    }
    {
        Bench b("trial", false, true);
        Phone* p = b.controller();
        CHECK(code(b.open(p, "echo")) == "busy");
    }
    printf("  a job holds the channels, and closes those of a package it changes; an agent on trial opens none\n");
}

// channel.open of a channel of chan, sent now and answered in the program's own time.
static std::string openInTime(Bench& b, Phone* p, const std::string& name) {
    int64_t id = p->request("channel.open", "{\"package\":\"org.nbtapp.chan\",\"channel\":\"" + name + "\"}");
    WAIT_FOR(b, p->replies.count(id) != 0);
    return p->reply(id);
}

static std::string pidOf(Bench& b, const std::string& name) {
    std::string text;
    if (!fs::readFile(b.agent->channels().pidFile("org.nbtapp.chan", name), 32, &text, 0)) return std::string();
    return text;
}

// Starting on demand: an open that starts the program, a second that starts none, the program's log, an agent that
// starts again with the program running, a program that never listens, and one that does not end at SIGTERM.
static void testStart() {
    {
        Bench b("start", true);
        Phone* p = b.controller();
        unsigned echo = openedOn(openInTime(b, p, "echo"));
        std::string pid = pidOf(b, "echo");
        CHECK_IN(pid, atol(pid.c_str()) > 1 && b.agent->channels().programRunning("org.nbtapp.chan", "echo"));
        p->send(echo, "started");
        std::vector<std::string> got;
        WAIT_FOR(b, collect(p, echo, &got) == 1);
        CHECK(got[0] == "started");
        // A second open starts nothing: the program takes it.
        unsigned second = openedOn(openInTime(b, p, "echo"));
        CHECK(second != echo && pidOf(b, "echo") == pid);
        // The program's output is its log, ${tmp}/channels/echo.log; it found its socket by what the agent gave it.
        std::string log;
        WAIT_FOR(b, fs::readFile(b.socketOf("echo") + ".log", 1 << 20, &log, 0) && has(log, "connection: ") &&
                        log.find("connection: ") != log.rfind("connection: "));
        CHECK_IN(log, has(log, "listening at " + b.socketOf("echo")) && log.find("listening at") == log.rfind("listening at"));

        // The agent starts again, with the program running: it knows the program, and starts no other.
        b.restart();
        CHECK(b.agent->channels().programRunning("org.nbtapp.chan", "echo"));
        // A mark of a process that is not the channel's program is not believed.
        CHECK(fs::writeFile(b.agent->channels().pidFile("org.nbtapp.chan", "feed"), num(getpid()) + "\n", 0600, 0));
        b.restart();
        CHECK(b.agent->channels().programRunning("org.nbtapp.chan", "echo"));
        CHECK(!b.agent->channels().programRunning("org.nbtapp.chan", "feed") && pidOf(b, "feed").empty());
        p = b.controller();
        unsigned again = openedOn(openInTime(b, p, "echo"));
        CHECK(pidOf(b, "echo") == pid);
        p->send(again, "again");
        got.clear();
        WAIT_FOR(b, collect(p, again, &got) == 1);

        // The log is kept short while the program runs: set aside at the cap, and the program writes on at the start
        // of the emptied file.
        CHECK(fs::appendFile(b.socketOf("echo") + ".log", std::string(kChannelLogCapBytes, 'x') + "\n", 0));
        b.step();
        CHECK(fs::kindOf(b.socketOf("echo") + ".log.1") == fs::kFile);
        openedOn(openInTime(b, p, "echo"));
        WAIT_FOR(b, fs::readFile(b.socketOf("echo") + ".log", 1 << 20, &log, 0) && has(log, "connection: "));
        CHECK_IN(log, log.size() < 4096 && log[0] != '\0');
    }
    {
        // A program that never takes the connection: here the stand-in for its socket refuses whatever runs. The
        // open is unavailable after kChannelStartWaitMs, and the program the open started is ended.
        Bench b("deaf");
        b.ends.refuse = true;
        Phone* p = b.controller();
        int64_t at = b.clock.now;
        std::string text = openInTime(b, p, "echo");
        CHECK_IN(text, code(text) == "unavailable" && has(text, "did not take the connection in 5 s"));
        CHECK(b.clock.now - at >= kChannelStartWaitMs);
        WAIT_FOR(b, !b.agent->channels().programRunning("org.nbtapp.chan", "echo"));
        CHECK(pidOf(b, "echo").empty());
    }
    {
        // A program that does not end at SIGTERM, which an earlier agent started: an upgrade of its package waits for
        // it, and it is sent SIGKILL kProgramKillAfterMs after SIGTERM.
        Bench b("kill", true);
        b.platform.starts = false;
        pid_t pid = b.run("chan_echo", "echo", "--ignore-term");
        CHECK(fs::writeFile(b.agent->channels().pidFile("org.nbtapp.chan", "echo"), num(pid) + "\n", 0600, 0));
        b.restart();
        CHECK(b.agent->channels().programRunning("org.nbtapp.chan", "echo"));
        Phone* p = b.controller();
        std::string planId = b.plan(p, offerOf("org.nbtapp.chan-1.1.0"));
        int64_t at = b.clock.now;
        CHECK(code(b.call(p, "job.start", "{\"planId\":\"" + planId + "\"}")) == "");
        WAIT_FOR(b, !p->events("job.done").empty());
        CHECK(b.clock.now - at >= kProgramKillAfterMs && b.clock.now - at < 3000);
        CHECK(kill(pid, 0) != 0 && !b.agent->channels().programRunning("org.nbtapp.chan", "echo"));
        CHECK(b.agent->engine().store().isInstalled("org.nbtapp.chan"));
    }
    printf("  a program started on demand, once; kept across a restart of the agent; ended when it does not listen\n");
}

static std::string setGated(Bench& b, Phone* p, bool on) {
    return b.call(p, "channel.set", std::string("{\"package\":\"org.nbtapp.chan\",\"channel\":\"gated\",\"on\":") + (on ? "true" : "false") + "}");
}

static std::string getChannel(Bench& b, Phone* p, const std::string& name) {
    return b.call(p, "channel.get", "{\"package\":\"org.nbtapp.chan\",\"channel\":\"" + name + "\"}");
}

// The switch: off by default; on, open, off (closed, the program gone); through a restart of the agent; the
// plugin's refusal and the others; removed with its package.
static void testSwitch() {
    Bench b("switch", true);
    Phone* p = b.controller();
    std::string file = switchFile(b.platform.runDir(), "org.nbtapp.chan", "gated");
    CHECK(has(getChannel(b, p, "gated"), "\"result\":{\"switched\":true,\"on\":false,\"open\":0}"));
    CHECK(has(getChannel(b, p, "echo"), "\"result\":{\"switched\":false,\"on\":true,\"open\":0}"));
    // Off: refused before anything is connected or started.
    std::string text = openInTime(b, p, "gated");
    CHECK_IN(text, code(text) == "off" && !b.agent->channels().programRunning("org.nbtapp.chan", "gated"));

    text = setGated(b, p, true);
    CHECK_IN(text, has(text, "\"result\":{\"switched\":true,\"on\":true,\"open\":0}") && fs::kindOf(file) == fs::kFile);
    unsigned gated = openedOn(openInTime(b, p, "gated"));
    CHECK(has(getChannel(b, p, "gated"), "\"on\":true,\"open\":1"));
    p->send(gated, "quiet");
    std::vector<std::string> got;
    WAIT_FOR(b, collect(p, gated, &got) == 1);
    CHECK(got[0] == "QUIET");   // the program's arguments: --upper

    // The agent starts again: the switch, in the run folder, is still on.
    b.restart();
    p = b.controller();
    CHECK(has(getChannel(b, p, "gated"), "\"switched\":true,\"on\":true"));
    gated = openedOn(openInTime(b, p, "gated"));

    // Off: the channel closes with `off`, and the program the agent started ends.
    Phone* plugin = b.controller("plugin");
    CHECK(code(setGated(b, plugin, false)) == "forbidden" && has(getChannel(b, plugin, "gated"), "\"on\":true"));
    text = setGated(b, p, false);
    CHECK_IN(text, has(text, "\"result\":{\"switched\":true,\"on\":false,\"open\":0}") && fs::kindOf(file) == fs::kNone);
    std::vector<std::string> closes = p->events("channel.close");
    CHECK(closes.size() == 1 && has(closes[0], "\"stream\":" + num(gated)) && has(closes[0], "\"reason\":\"off\""));
    WAIT_FOR(b, !b.agent->channels().programRunning("org.nbtapp.chan", "gated"));
    CHECK(code(setGated(b, p, false)) == "");   // off again: nothing to do

    // The others: no switch, no such channel, not a boolean.
    CHECK(code(b.call(p, "channel.set", "{\"package\":\"org.nbtapp.chan\",\"channel\":\"echo\",\"on\":false}")) == "bad-request");
    CHECK(code(b.call(p, "channel.set", "{\"package\":\"org.nbtapp.chan\",\"channel\":\"none\",\"on\":true}")) == "not-found");
    CHECK(code(b.call(p, "channel.set", "{\"package\":\"org.nbtapp.chan\",\"channel\":\"gated\",\"on\":1}")) == "bad-request");
    CHECK(code(getChannel(b, p, "none")) == "not-found");

    // While a job runs, nothing is switched: one that waits for its archive here.
    std::string planId = b.plan(p, offerOf("org.nbtapp.radio-1.0.0"));
    p->holdFetches = true;
    CHECK(code(b.call(p, "job.start", "{\"planId\":\"" + planId + "\"}")) == "");
    CHECK(code(setGated(b, p, true)) == "busy" && fs::kindOf(file) == fs::kNone);
    p->holdFetches = false;
    for (size_t i = 0; i < p->held.size(); ++i) p->serve(p->held[i]);
    p->held.clear();
    WAIT_FOR(b, !p->events("job.done").empty());

    // The package goes, and its switch with it.
    CHECK(code(setGated(b, p, true)) == "" && fs::kindOf(file) == fs::kFile);
    text = b.call(p, "job.plan", "{\"install\":[],\"remove\":[{\"id\":\"org.nbtapp.chan\",\"deleteData\":true}],\"offers\":[]}");
    json::Document doc;
    CHECK_IN(text, doc.parse(text, 0) && doc.root().find("result").find("planId").isString());
    CHECK(code(b.call(p, "job.start", "{\"planId\":\"" + doc.root().find("result").find("planId").string() + "\"}")) == "");
    WAIT_FOR(b, !b.agent->engine().store().isInstalled("org.nbtapp.chan"));
    b.settle();
    CHECK(fs::kindOf(file) == fs::kNone);
    printf("  the switch: off by default, on, off, kept through a restart, gone with its package\n");

    // An agent on trial switches nothing.
    Bench t("switch-trial", false, true);
    Phone* q = t.controller();
    CHECK(code(setGated(t, q, true)) == "busy");
}

static bool programRunning(Bench& b, const std::string& name) { return b.agent->channels().programRunning("org.nbtapp.chan", name); }

// A program that is a script whose process stays the shell: here chan's bin/chan_echo runs chan_echo and waits for
// it, in its own group. An agent that starts again knows it by its interpreter, and a switch off, or a job of its
// package, ends the whole group. A mark of a shell that does not lead a group of its own is not believed.
static void testScript() {
    Bench b("script", true);
    std::string why, script = b.platform.appDir("org.nbtapp.chan") + "/bin/chan_echo";
    CHECK_IN(why, fs::writeFile(script, "#!/bin/sh\n" + gPrograms + "/chan_echo \"$@\"\nexit $?\n", 0755, &why));
    Phone* p = b.controller();
    CHECK(code(setGated(b, p, true)) == "");
    unsigned gated = openedOn(openInTime(b, p, "gated"));
    openedOn(openInTime(b, p, "echo"));
    pid_t gatedPid = (pid_t)atol(pidOf(b, "gated").c_str()), echoPid = (pid_t)atol(pidOf(b, "echo").c_str());
    CHECK(gatedPid > 1 && echoPid > 1 && gatedPid != echoPid);
    CHECK_IN(exePathOf(gatedPid), programName(exePathOf(gatedPid).c_str()) != "chan_echo");
    p->send(gated, "script");
    std::vector<std::string> got;
    WAIT_FOR(b, collect(p, gated, &got) == 1);
    CHECK(got[0] == "SCRIPT");

    // A shell, as /bin/sh runs it, in this process's group, marked as feed's program, whose script names /bin/sh.
    char real[PATH_MAX];
    CHECK(realpath("/bin/sh", real) != 0);
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t shell = fork();
    CHECK(shell >= 0);
    if (shell == 0) {
        dup2(fds[0], 0);
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        execl("/bin/sh", "sh", "-c", "read line; exit 0", (char*)0);
        _exit(127);
    }
    close(fds[0]);
    b.programs.push_back(shell);
    for (int i = 0; i < 500 && programName(exePathOf(shell).c_str()) != programName(real); ++i) usleep(10000);
    CHECK_IN(exePathOf(shell), programName(exePathOf(shell).c_str()) == programName(real));
    CHECK(fs::writeFile(b.agent->channels().pidFile("org.nbtapp.chan", "feed"), num(shell) + "\n", 0600, 0));

    b.restart();
    CHECK(programRunning(b, "gated") && programRunning(b, "echo"));
    CHECK(!programRunning(b, "feed") && pidOf(b, "feed").empty());
    close(fds[1]);
    p = b.controller();
    // Off: the shell and the chan_echo it runs end.
    CHECK(code(setGated(b, p, false)) == "");
    WAIT_FOR(b, !programRunning(b, "gated") && kill(-gatedPid, 0) != 0);
    CHECK(programRunning(b, "echo"));
    // A job of its package: the same.
    std::string planId = b.plan(p, offerOf("org.nbtapp.chan-1.1.0"));
    CHECK(code(b.call(p, "job.start", "{\"planId\":\"" + planId + "\"}")) == "");
    WAIT_FOR(b, !p->events("job.done").empty());
    WAIT_FOR(b, !programRunning(b, "echo") && kill(-echoPid, 0) != 0);
    printf("  a program that is a script, its process the shell, known again after a restart and ended whole\n");
}

// channel.open of a channel of chan, sent now; the request's id.
static int64_t openLater(Phone* p, const std::string& name) {
    return p->request("channel.open", "{\"package\":\"org.nbtapp.chan\",\"channel\":\"" + name + "\"}");
}

// How many answers to request `id` the phone has had, taken or not.
static int answersTo(const Phone* p, int64_t id) {
    int n = 0;
    for (size_t i = 0; i < p->heard.size(); ++i) n += p->heard[i] == "R" + num(id) ? 1 : 0;
    return n;
}

// The clock goes past the time an open waits for its program, and the agent takes its turns.
static void pastStartWait(Bench& b) {
    b.clock.now += kChannelStartWaitMs + 1000;
    b.settle();
}

static unsigned openCount(Bench& b, const std::string& name) { return b.agent->channels().count("org.nbtapp.chan", name); }

// An open that is cut short while it waits for its program is answered, once: switched off, with off; a job of any
// package starts, with busy at once, and a program the open started is ended; a job of its own package. An open that
// opens, and one whose program never takes the connection, are answered once too; one whose link goes, not at all.
static void testCutShort() {
    {
        Bench b("cut-off");
        Phone* p = b.controller();
        CHECK(code(setGated(b, p, true)) == "");
        b.ends.wait = true;
        int64_t id = openLater(p, "gated");
        b.settle();
        CHECK(p->replies.count(id) == 0 && openCount(b, "gated") == 1);
        Phone* other = b.controller();
        CHECK(code(setGated(b, other, false)) == "");
        std::string text = p->reply(id);
        CHECK_IN(text, code(text) == "off" && has(text, "switched off"));
        CHECK(openCount(b, "gated") == 0 && p->events("channel.close").empty());
        pastStartWait(b);
        CHECK(answersTo(p, id) == 1);
    }
    {
        // Nothing listens, so the agent starts the program; the stand-in for its socket refuses whatever runs.
        Bench b("cut-job");
        Phone* p = b.controller();
        b.ends.refuse = true;
        int64_t id = openLater(p, "echo");
        b.settle();
        CHECK(p->replies.count(id) == 0 && b.agent->channels().programRunning("org.nbtapp.chan", "echo"));
        std::string planId = b.plan(p, offerOf("org.nbtapp.radio-1.0.0"));
        p->holdFetches = true;
        int64_t start = p->request("job.start", "{\"planId\":\"" + planId + "\"}");
        b.settle();
        std::string started = p->reply(start);
        CHECK_IN(started, code(started) == "");
        std::string text = p->reply(id);
        CHECK_IN(text, code(text) == "busy");
        CHECK(heardAt(p, "R" + num(id)) > heardAt(p, "R" + num(start)));
        // Nothing is open on the link, so it is not told the channels are held.
        CHECK(openCount(b, "echo") == 0 && p->events("channel.paused").empty());
        WAIT_FOR(b, !b.agent->channels().programRunning("org.nbtapp.chan", "echo"));
        CHECK(pidOf(b, "echo").empty());
        p->holdFetches = false;
        for (size_t i = 0; i < p->held.size(); ++i) p->serve(p->held[i]);
        p->held.clear();
        WAIT_FOR(b, !p->events("job.done").empty());
        pastStartWait(b);
        CHECK(answersTo(p, id) == 1 && openCount(b, "echo") == 0 && p->events("channel.resumed").empty());
        CHECK(b.agent->engine().store().isInstalled("org.nbtapp.radio"));
    }
    {
        // A job of the channel's own package.
        Bench b("cut-upgrade");
        Phone* p = b.controller();
        b.ends.wait = true;
        int64_t id = openLater(p, "echo");
        b.settle();
        CHECK(p->replies.count(id) == 0);
        std::string planId = b.plan(p, offerOf("org.nbtapp.chan-1.1.0"));
        CHECK(code(b.call(p, "job.start", "{\"planId\":\"" + planId + "\"}")) == "");
        std::string text = p->reply(id);
        CHECK_IN(text, code(text) == "busy");
        WAIT_FOR(b, !p->events("job.done").empty());
        pastStartWait(b);
        CHECK(answersTo(p, id) == 1 && openCount(b, "echo") == 0 && p->events("channel.close").empty());
    }
    {
        Bench b("once");
        Phone* p = b.controller();
        int64_t id = openLater(p, "echo");
        b.settle();
        CHECK(code(p->reply(id)) == "");
        pastStartWait(b);
        CHECK(answersTo(p, id) == 1 && openCount(b, "echo") == 1);
        // Never taken: unavailable once its time is up.
        b.ends.wait = true;
        id = openLater(p, "feed");
        b.settle();
        CHECK(p->replies.count(id) == 0);
        pastStartWait(b);
        std::string text = p->reply(id);
        CHECK_IN(text, code(text) == "unavailable");
        pastStartWait(b);
        CHECK(answersTo(p, id) == 1 && openCount(b, "feed") == 0);
        // The link goes while its open waits: nobody is answered, and nothing is left.
        Phone* gone = b.controller();
        openLater(gone, "echo");
        b.settle();
        CHECK(openCount(b, "echo") == 2);
        gone->hangUp();
        b.settle();
        CHECK(openCount(b, "echo") == 1);
    }
    printf("  an open cut short while it waits is answered once: off, busy at a job's start\n");
}

// A host for a table of channels on its own, which keeps the lines the table has for the log.
class NoteHost : public ChannelHost {
public:
    NoteHost() : now(1000) {}
    virtual void channelConnected(Channel*) {}
    virtual void channelRefused(Channel*, const char*, const std::string&) {}
    virtual void channelClosed(Channel*, const char*, const std::string&) {}
    virtual int64_t channelNowMs() { return now; }
    virtual void channelNote(const std::string& line) { notes.push_back(line); }
    // How many of the lines say `part`.
    size_t said(const std::string& part) const {
        size_t n = 0;
        for (size_t i = 0; i < notes.size(); ++i) n += has(notes[i], part) ? 1 : 0;
        return n;
    }
    int64_t now;
    std::vector<std::string> notes;
};

static long long sizeOf(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long long)st.st_size : -1;
}

// The log cap when the log cannot be set aside, as on a full RAM disk: here <log>.1 is a folder. The log is emptied
// all the same, and that is said once, not on every turn; once the log has been set aside again, a failure is said
// again.
static void testLogCapFails() {
    std::string dir = shortFolder("logcap"), why, log = dir + "/echo.log";
    NoteHost host;
    Channels table(&host, 0);
    table.setPrograms(dir, "/bin/sh");
    // The program is this process, which the table only looks at here: nothing ends it.
    table.adoptProgram("org.nbtapp.chan", "echo", (long)getpid(), log);
    const std::string over(kChannelLogCapBytes + 1, 'x'), failed = "could not be set aside", setAside = "what it held is in";
    CHECK_IN(why, fs::makeDirs(log + ".1", &why));
    for (int round = 0; round < 2; ++round) {
        CHECK(fs::writeFile(log, over, 0644, 0));
        table.tick();
        CHECK(sizeOf(log) == 0);
        for (int i = 0; i < 5; ++i) table.tick();
    }
    CHECK(host.said(failed) == 1 && host.said(setAside) == 0);
    // Set aside again: said; and the next failure is said again.
    CHECK_IN(why, fs::removeTree(log + ".1", &why));
    CHECK(fs::writeFile(log, over, 0644, 0));
    table.tick();
    CHECK(sizeOf(log) == 0 && sizeOf(log + ".1") == (long long)over.size() && host.said(setAside) == 1);
    CHECK_IN(why, fs::removeFile(log + ".1", &why) && fs::makeDirs(log + ".1", &why));
    CHECK(fs::writeFile(log, over, 0644, 0));
    table.tick();
    table.tick();
    CHECK(sizeOf(log) == 0 && host.said(failed) == 2);
    CHECK(fs::removeTree(dir, &why));
    printf("  a log that cannot be set aside is emptied all the same, and that is said once\n");
}

// A package whose channel's socket would not fit a local socket's address is refused at install.
static void testLongPath() {
    std::string root = gWork + "/" + std::string(100, 'r');
    HostPlatform platform(root);
    std::string why;
    CHECK_IN(why, fs::removeTree(root, &why) && platform.prepare(&why));
    Engine engine(&platform, keys(), "1.7.0");
    CHECK_IN(why, engine.init(&why));
    StepReport r = engine.install(gData + "/good/org.nbtapp.chan-1.0.0.nbtpkg", "", 0);
    CHECK_IN(r.error.message, r.outcome == kStepFailed && r.error.code == "bad-manifest" && has(r.error.message, "too long"));
    CHECK(fs::removeTree(root, &why));
    printf("  a channel whose socket's path is too long is refused at install\n");
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: test_channels TESTDATA WORK\n");
        return 2;
    }
    gData = argv[1];
    gPrograms = fs::parentOf(argv[0]);
    // Absolute, as nbtagent makes its root: a program is started in a folder of its own, by a path under the root.
    std::string why;
    CHECK_IN(why, fs::makeDirs(argv[2], &why));
    char* work = realpath(argv[2], 0);
    CHECK(work != 0);
    gWork = work;
    free(work);
    std::string text = testutil::readFile(gData + "/vectors.json");
    json::Document doc;
    CHECK(doc.parse(text, 0));
    gKey = testutil::key(doc.root().find("keys").find("package").string(), "test package key", true);
    signal(SIGPIPE, SIG_IGN);

    testOpen();
    testRefusals();
    testFaults();
    testStreamNumbers();
    testSocket();
    testSlowReader();
    testCredits();
    testTurns();
    testJobs();
    testStart();
    testSwitch();
    testScript();
    testCutShort();
    testLogCapFails();
    testLongPath();
    printf("test_channels: ok\n");
    return 0;
}

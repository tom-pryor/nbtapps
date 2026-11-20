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
    explicit Phone(int fd) : maxFrame(65536), closed(false), fd_(fd), nextId_(1) {
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
    void handle(const std::string& text) {
        json::Document doc;
        CHECK_IN(text, doc.parse(text, 0));
        json::Value root = doc.root();
        if (root.find("method").isString()) {
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
    std::string out_, in_;
    std::vector<std::string> events_;
};

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
    StandIns() : refuse(false) {}
    ~StandIns() {
        for (size_t i = 0; i < records.size(); ++i) delete records[i];
    }
    virtual ProgramEnd* make(const std::string& path) {
        EndRecord* r = new EndRecord();
        r->path = path;
        records.push_back(r);
        if (refuse) return new Refusing();
        return new StandInEnd(r);
    }
    EndRecord* last() { return records.empty() ? 0 : records.back(); }

    bool refuse;   // the next ends find nothing listening
    std::vector<EndRecord*> records;

private:
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

// A local socket's path has a limit of about 100 bytes, which a work folder deep in a checkout passes: the head unit
// of a test is in a short folder of its own under /tmp (the bench's container has its own /tmp).
static std::string shortRoot(const std::string& name) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/nbtch-%s-XXXXXX", name.c_str());
    CHECK_IN(name, mkdtemp(path) != 0);
    return path;
}

struct Bench {
    std::string root;
    HostPlatform platform;
    FakeClock clock;
    FakeRandom random;
    StandIns ends;
    Agent agent;
    std::vector<Phone*> phones;
    std::vector<pid_t> programs;   // the ones the test started

    // With `sockets`, the programs' ends are their sockets, as on a head unit; otherwise stand-ins.
    explicit Bench(const std::string& name, bool sockets = false)
        : root(shortRoot(name)), platform(root), agent(&platform, keys(), "1.7.0", &clock, &random) {
        std::string why;
        CHECK_IN(why, platform.prepare(&why));
        agent.setLog(0);
        CHECK_IN(why, agent.init(&why));
        if (!sockets) agent.setProgramEnds(&ends);
        StepReport r = agent.engine().install(gData + "/good/org.nbtapp.chan-1.0.0.nbtpkg", "", 0);
        CHECK_IN(r.error.code + ": " + r.error.message, r.outcome == kStepDone);
    }
    ~Bench() {
        for (size_t i = 0; i < phones.size(); ++i) delete phones[i];
        for (size_t i = 0; i < programs.size(); ++i) {
            kill(programs[i], SIGKILL);
            waitpid(programs[i], 0, 0);
        }
        fs::removeTree(root, 0);
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

    // One turn that waits a little for what a program sends.
    void step() {
        for (size_t i = 0; i < phones.size(); ++i) phones[i]->pump();
        agent.turn(5);
        for (size_t i = 0; i < phones.size(); ++i) phones[i]->pump();
    }

    Phone* connect(const char* kind) {
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        agent.adopt(new SocketLink(fds[0], kind));
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
            agent.turn(0);
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
    CHECK(b.agent.channels().count("org.nbtapp.chan", "echo") == 1);

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
    CHECK(!r->alive && b.agent.channels().count("org.nbtapp.chan", "echo") == 0 && p->events("channel.close").empty());

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
    CHECK(!r->alive && b.agent.channels().count("org.nbtapp.chan", "echo") == 0);
    printf("  faults close their channel, a lost link closes its channels\n");
}

// A files stream's numbers run to kStreamLast, and start again at 1: the numbers above are the channels'.
static void testStreamNumbers() {
    Bench b("numbers");
    Phone* p = b.controller();
    std::string log = b.root + "/agent.log";
    CHECK(fs::writeFile(log, "x\n", 0644, 0));
    b.agent.setOwnLog(log);
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
    b.agent.setOwnLog(log);
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
    gWork = argv[2];
    gPrograms = fs::parentOf(argv[0]);
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
    testLongPath();
    printf("test_channels: ok\n");
    return 0;
}

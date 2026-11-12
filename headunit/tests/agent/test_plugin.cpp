// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for the plugin's side of pairing: src/agent_link.hpp, src/phone_screens.hpp and src/crc32.hpp, which
// are the parts of libNbtApp.so that need no HU Framework. The screens run here against a real Agent, over a
// real local socket, with a view that writes down what it is told to show; a phone on a link of the serial kind
// types the code, and what Start the agent starts is an agent made by the test. What the HMI does with a text is not tested here, or anywhere but on a car.
//
// One thread, as in the HMI: a screen's tick, then a turn of the agent, until both have had their say.
//
// usage: test_plugin [work folder]

#include "../../src/phone_screens.hpp"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <vector>

#include "../../agent/agent.hpp"
#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "../../agent/inflate.hpp"
#include "../../agent/json_text.hpp"
#include "../../src/crc32.hpp"
#include "test_util.hpp"

using namespace nbt;

// ---- what the test puts around the screens --------------------------------------------------------------------------

class FakeClock : public Clock {
public:
    FakeClock() : now(1000) {}
    virtual int64_t nowMs() { return now; }
    virtual int64_t unixSeconds() { return 1794400000; }
    int64_t now;
};

class CountRandom : public RandomSource {
public:
    CountRandom() : next(0) {}
    virtual void bytes(uint8_t* out, size_t len) {
        for (size_t i = 0; i < len; ++i) out[i] = next++;
    }
    virtual bool secret(uint8_t* out, size_t len) {
        bytes(out, len);
        return true;
    }
    uint8_t next;
};

// What the HMI would be told to show, in order: "code 123456", "done", ...
class View : public PairView, public ForgetView, public StartView {
public:
    virtual void pairShow(PairStatus status, const std::string& code) {
        static const char* const names[] = {"asking", "code", "done", "wrong-codes", "ran-out", "too-soon", "no-agent", "failed"};
        shown.push_back(std::string(names[status]) + (code.empty() ? "" : " " + code));
        // A code comes with kPairCode and with nothing else.
        CHECK((status == kPairCode) == (code.size() == kPairingCodeDigits));
    }
    virtual void forgetShow(ForgetStatus status) {
        static const char* const names[] = {"ask", "working", "done", "no-agent", "failed"};
        shown.push_back(std::string("forget ") + names[status]);
    }
    virtual void startShow(StartStatus status) {
        static const char* const names[] = {"checking", "running", "ask", "starting", "started", "no-launcher", "failed"};
        shown.push_back(std::string("start ") + names[status]);
    }
    // Everything shown since the last call, joined by ", ".
    std::string take() {
        std::string all;
        for (size_t i = 0; i < shown.size(); ++i) all += (i == 0 ? "" : ", ") + shown[i];
        shown.clear();
        return all;
    }
    std::vector<std::string> shown;
};

// A phone's end of a link: what the agent answered, by request.
class Replies : public PeerHandler {
public:
    virtual void onRequest(Peer* peer, int64_t id, const std::string&, const json::Value&) { peer->respond(id, "{}"); }
    virtual void onResponse(Peer*, int64_t id, bool ok, const json::Value& result, const std::string& code, const std::string&) {
        got[id] = ok ? "ok " + jsonText(result) : code;
    }
    virtual void onEvent(Peer*, const std::string&, int64_t, const json::Value&) {}
    virtual void onData(Peer*, uint16_t, const std::string&, bool) {}
    std::map<int64_t, std::string> got;
};

static std::string gWork, gSocketDir;

// A head unit in a folder, with its agent listening on a local socket.
struct Car {
    std::string socket;
    HostPlatform platform;
    FakeClock clock;
    CountRandom random;
    Agent agent;
    LocalListener listener;
    Replies replies;
    Peer* phone;

    explicit Car(const std::string& name)
        : socket(gSocketDir + "/" + name + ".sock"), platform(gWork + "/" + name),
          agent(&platform, std::vector<TrustedKey>(), "1.0.0", &clock, &random), phone(0) {
        std::string why;
        CHECK_IN(why, fs::removeTree(gWork + "/" + name, &why) && platform.prepare(&why));
        agent.setLog(0);
        CHECK_IN(why, agent.init(&why));
        CHECK_IN(why, listener.open(socket, &why));
        agent.addListener(&listener);
    }
    ~Car() { delete phone; }

    // The screens' ticks and the agent's turns, until all of them have had what the others sent.
    void run(PairScreen* pair, ForgetScreen* forget = 0, PairScreen* other = 0) {
        for (int round = 0; round < 12; ++round) {
            if (pair != 0) pair->tick();
            if (other != 0) other->tick();
            if (forget != 0) forget->tick();
            agent.turn(0);
            if (phone != 0 && phone->open()) {
                phone->flush();
                phone->readable();
            }
        }
    }

    // A phone on a link of the serial kind, which has said hello.
    void connectPhone() {
        delete phone;
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        agent.adopt(new SocketLink(fds[0], "serial"));
        phone = new Peer(new SocketLink(fds[1], "phone"), true, &replies);
        CHECK(ask("hello", "{\"protocolMin\":1,\"protocolMax\":1,\"role\":\"phone\",\"phoneId\":\"11111111111111111111111111111111\","
                           "\"name\":\"Test phone\",\"maxFrame\":4096,\"capabilities\":[]}").compare(0, 3, "ok ") == 0);
    }
    // A request of the phone's, and its answer: "ok {...}", or the error's code.
    std::string ask(const std::string& method, const std::string& params, PairScreen* pair = 0) {
        int64_t id = phone->request(method, params);
        CHECK(id != 0);
        run(pair);
        CHECK_IN(method, replies.got.count(id) != 0);
        return replies.got[id];
    }
    std::string type(const std::string& code, PairScreen* pair = 0) { return ask("pairing.issue", "{\"code\":\"" + code + "\"}", pair); }
};

// What Start the agent starts: what the test says it came to, and nothing more.
class FakeStarter : public AgentStarter {
public:
    FakeStarter() : result(kLaunched), calls(0) {}
    virtual LaunchResult startAgent() {
        ++calls;
        return result;
    }
    LaunchResult result;
    int calls;
};

// The screen's ticks, and the agent's turns where there is one.
static void turns(StartScreen* screen, Car* car, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        screen->tick();
        if (car != 0) car->agent.turn(0);
    }
}

// The code in "…, code 123456".
static std::string codeIn(const std::string& shown) {
    size_t at = shown.rfind("code ");
    CHECK_IN(shown, at != std::string::npos && shown.size() == at + 5 + kPairingCodeDigits);
    return shown.substr(at + 5);
}

static std::string wrong(std::string code) {
    code[0] = code[0] == '9' ? '0' : (char)(code[0] + 1);
    return code;
}

// ---- the tests ----------------------------------------------------------------------------------------------------

static void testCrc() {
    // The plugin's CRC32 is zlib's, which is what the agent checks its frames with.
    CHECK(plugin::crc32(0, "", 0) == 0 && plugin::crc32(0, "123456789", 9) == 0xCBF43926u);
    std::string data;
    for (int i = 0; i < 5000; ++i) data += (char)(i * 131 + (i >> 3));
    CHECK(plugin::crc32(0, data.data(), data.size()) == nbt::crc32(0, data.data(), data.size()));
    uint32_t parts = plugin::crc32(plugin::crc32(0, data.data(), 1234), data.data() + 1234, data.size() - 1234);
    CHECK(parts == nbt::crc32(0, data.data(), data.size()));
}

static void testPair() {
    Car car("pair");
    View view;
    PairScreen screen(car.socket, &view);
    car.connectPhone();
    CHECK(!screen.wantsTick() && car.type("123456") == "forbidden");

    // Shown: the agent is asked, and its code is on the screen. The window is open for as long as it is.
    screen.shown();
    CHECK(view.take() == "asking" && screen.wantsTick());
    car.run(&screen);
    std::string code = codeIn(view.take());
    CHECK(screen.status() == kPairCode && screen.wantsTick());
    CHECK(car.type(wrong(code), &screen) == "rejected" && view.take().empty());
    // The phone types it: paired, the screen says so, and the plugin's link is gone.
    CHECK(car.type(code, &screen).compare(0, 11, "ok {\"key\":\"") == 0);
    car.run(&screen);
    CHECK(view.take() == "done" && !screen.wantsTick() && car.agent.phones().all().size() == 1);
    car.run(&screen);
    CHECK(car.agent.sessions() == 1);   // the phone's

    // Left before anybody typed it: the code is good for nothing, and the screen says nothing more.
    screen.shown();
    car.run(&screen);
    code = codeIn(view.take());
    screen.hidden();
    CHECK(!screen.wantsTick());
    car.run(&screen);
    CHECK(car.type(code) == "forbidden" && view.take().empty() && car.agent.sessions() == 1);
    screen.hidden();   // twice is harmless

    // Shown again while it is shown: a new code, and the old one is void.
    screen.shown();
    car.run(&screen);
    code = codeIn(view.take());
    screen.shown();
    car.run(&screen);
    std::string newer = codeIn(view.take());
    CHECK(newer != code && car.type(code, &screen) == "rejected");
    // Three wrong codes: the screen says so, and the next showing that it is too soon. After the pause, a code.
    CHECK(car.type(wrong(newer), &screen) == "rejected" && view.take().empty());
    CHECK(car.type(wrong(newer), &screen) == "rejected");
    car.run(&screen);
    CHECK(view.take() == "wrong-codes" && !screen.wantsTick());
    screen.shown();
    car.run(&screen);
    CHECK(view.take() == "asking, too-soon" && !screen.wantsTick());
    car.clock.now += kPairingPauseSeconds * 1000;
    screen.shown();
    car.run(&screen);
    code = codeIn(view.take());

    // Nobody types it: after 120 s it has run out.
    car.clock.now += kPairingWindowSeconds * 1000;
    car.run(&screen);
    CHECK(view.take() == "ran-out" && !screen.wantsTick() && car.type(code) == "forbidden");

    // Another controller opens a window (nbtctl pair, say): this screen's code is void, and it says so.
    View otherView;
    PairScreen other(car.socket, &otherView);
    screen.shown();
    car.run(&screen);
    code = codeIn(view.take());
    other.shown();
    car.run(&screen, 0, &other);
    CHECK(view.take() == "ran-out" && codeIn(otherView.take()) != code);
    other.hidden();
    car.run(&screen, 0, &other);
    CHECK(car.agent.sessions() == 1);
}

static void testForget() {
    Car car("forget");
    View view;
    ForgetScreen screen(car.socket, &view);
    for (char digit = '1'; digit <= '3'; ++digit) {
        PairedPhone phone;
        phone.id = std::string(32, digit);
        phone.name = "Test phone";
        std::string why;
        CHECK_IN(why, car.agent.phones().put(phone, &why));
    }
    // It asks first, and nothing is sent until the row is pressed.
    screen.shown();
    car.run(0, &screen);
    CHECK(view.take() == "forget ask" && !screen.wantsTick() && car.agent.phones().all().size() == 3);
    screen.confirmed();
    CHECK(view.take() == "forget working" && screen.wantsTick());
    car.run(0, &screen);
    CHECK(view.take() == "forget done" && !screen.wantsTick() && car.agent.phones().all().empty());
    car.run(0, &screen);
    CHECK(car.agent.sessions() == 0);
    // A second press does nothing; shown again, it asks again.
    screen.confirmed();
    CHECK(view.take().empty() && !screen.wantsTick());
    screen.shown();
    screen.confirmed();
    car.run(0, &screen);
    CHECK(view.take() == "forget ask, forget working, forget done");
}

// Start the agent: one that is running, one that is not installed, one that comes a while after its start, and
// one that does not come.
static void testStart() {
    View view;
    FakeStarter starter;
    {   // Running: there is nothing to start, and a press does nothing.
        Car car("start-running");
        StartScreen screen(car.socket, &view, &starter);
        screen.shown();
        CHECK(view.take() == "start checking" && screen.wantsTick());
        turns(&screen, &car, 12);
        CHECK(view.take() == "start running" && !screen.wantsTick());
        screen.pressed();
        CHECK(view.take().empty() && starter.calls == 0);
        screen.hidden();
    }
    const std::string nobody = gSocketDir + "/nobody.sock";
    {   // Not running, and not installed. Once said, a press does nothing.
        StartScreen screen(nobody, &view, &starter);
        screen.shown();
        CHECK(view.take() == "start checking, start ask" && !screen.wantsTick());
        starter.result = kLaunchNoLauncher;
        screen.pressed();
        CHECK(view.take() == "start no-launcher" && starter.calls == 1 && !screen.wantsTick());
        screen.pressed();
        CHECK(view.take().empty() && starter.calls == 1);
    }
    {   // Started, and its agent comes a few seconds later: the screen asks once a second until it answers.
        Car* late = 0;
        StartScreen screen(gSocketDir + "/start-late.sock", &view, &starter);
        screen.shown();
        CHECK(view.take() == "start checking, start ask");
        starter.result = kLaunched;
        screen.pressed();
        CHECK(view.take() == "start starting" && starter.calls == 2 && screen.wantsTick());
        turns(&screen, 0, 3 * kStartProbeTicks);
        CHECK(view.take().empty() && screen.wantsTick());
        late = new Car("start-late");
        turns(&screen, late, 2 * kStartProbeTicks);
        CHECK(view.take() == "start started" && !screen.wantsTick());
        // Shown again, it is running.
        screen.shown();
        turns(&screen, late, 12);
        CHECK(view.take() == "start checking, start running");
        delete late;
    }
    {   // A launcher was running already, and no agent answers in time; a press after that tries again.
        StartScreen screen(nobody, &view, &starter);
        screen.shown();
        starter.result = kLaunchRunning;
        screen.pressed();
        CHECK(view.take() == "start checking, start ask, start starting");
        turns(&screen, 0, kStartWaitTicks - 1);
        CHECK(view.take().empty() && screen.wantsTick());
        turns(&screen, 0, 1);
        CHECK(view.take() == "start failed" && !screen.wantsTick());
        starter.result = kLaunchFailed;
        screen.pressed();
        CHECK(view.take() == "start failed" && starter.calls == 4);
        starter.result = kLaunched;
        screen.pressed();
        CHECK(view.take() == "start starting" && starter.calls == 5 && screen.wantsTick());
        // Left while it waits: the screen stops asking, and comes back asking anew.
        screen.hidden();
        CHECK(!screen.wantsTick() && screen.status() == kStartAsk);
        screen.shown();
        CHECK(view.take() == "start checking, start ask" && !screen.wantsTick());
    }
}

// An agent that is not there, one that goes, one that says nothing, and one that will not have the plugin.
static void testNoAgent() {
    View view;
    {
        PairScreen pair(gSocketDir + "/nobody.sock", &view);
        pair.shown();
        CHECK(view.take() == "asking, no-agent" && !pair.wantsTick());
        pair.tick();
        pair.hidden();
        ForgetScreen forget(gSocketDir + "/nobody.sock", &view);
        forget.shown();
        forget.confirmed();
        CHECK(view.take() == "forget ask, forget no-agent" && !forget.wantsTick());
    }
    std::string why, path = gSocketDir + "/mute.sock";
    LocalListener mute;
    CHECK_IN(why, mute.open(path, &why));
    {   // It takes the link and says nothing: after 5 s of ticks the screen gives up.
        PairScreen pair(path, &view);
        pair.shown();
        Link* held = mute.accept();
        CHECK(held != 0);
        for (unsigned i = 0; i < kScreenAnswerTicks; ++i) pair.tick();
        CHECK(view.take() == "asking" && pair.wantsTick());
        pair.tick();
        CHECK(view.take() == "no-agent" && !pair.wantsTick());
        delete held;
        ForgetScreen forget(path, &view);
        forget.shown();
        forget.confirmed();
        held = mute.accept();
        for (unsigned i = 0; i <= kScreenAnswerTicks; ++i) forget.tick();
        CHECK(view.take() == "forget ask, forget working, forget no-agent" && !forget.wantsTick());
        delete held;
    }
    {   // It goes while the screen waits. The write that follows raises no signal: the HMI's process has to live.
        struct sigaction fatal, before;
        memset(&fatal, 0, sizeof fatal);
        fatal.sa_handler = SIG_DFL;
        sigaction(SIGPIPE, &fatal, &before);   // the default, which ends the process: as it may be in the HMI
        PairScreen pair(path, &view);
        pair.shown();
        delete mute.accept();
        pair.hidden();   // pairing.close, written to a socket whose other end is closed
        pair.shown();
        delete mute.accept();
        pair.tick();
        pair.tick();
        CHECK(view.take() == "asking, asking, no-agent" && !pair.wantsTick());
        sigaction(SIGPIPE, &before, 0);
    }
    {   // It answers hello with an error: no agent for this plugin.
        struct Refuser : public PeerHandler {
            virtual void onRequest(Peer* peer, int64_t id, const std::string&, const json::Value&) { peer->respondError(id, "unsupported-version", "no"); }
            virtual void onResponse(Peer*, int64_t, bool, const json::Value&, const std::string&, const std::string&) {}
            virtual void onEvent(Peer*, const std::string&, int64_t, const json::Value&) {}
            virtual void onData(Peer*, uint16_t, const std::string&, bool) {}
        };
        PairScreen pair(path, &view);
        pair.shown();
        Refuser refuser;
        Peer refusing(mute.accept(), false, &refuser);
        refusing.readable();
        refusing.flush();
        pair.tick();
        pair.tick();
        CHECK(view.take() == "asking, no-agent" && !pair.wantsTick());
    }
}

int main(int argc, char** argv) {
    // The agent's own links are written with write(): the agent ignores SIGPIPE, and so does this test for it.
    signal(SIGPIPE, SIG_IGN);
    std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host") + "/work-plugin";
    char cwd[4096];
    CHECK(getcwd(cwd, sizeof cwd) != 0);
    gWork = work[0] == '/' ? work : std::string(cwd) + "/" + work;
    std::string why;
    CHECK_IN(why, fs::removeTree(gWork, &why) && fs::makeDirs(gWork, &why));
    fs::setSyncing(false);
    // The sockets go where a path is short: a local socket's has about 100 bytes.
    char dir[64];
    snprintf(dir, sizeof dir, "/tmp/nbt_plugin_%ld", (long)getpid());
    gSocketDir = dir;
    CHECK_IN(why, fs::removeTree(gSocketDir, &why) && mkdir(dir, 0700) == 0);

    testCrc();
    testPair();
    testForget();
    testStart();
    testNoAgent();

    fs::removeTree(gSocketDir, 0);
    fs::removeTree(gWork, 0);
    printf("test_plugin: ok\n");
    return 0;
}

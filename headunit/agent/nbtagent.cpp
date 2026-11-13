// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// nbtagent: the agent as a program. It listens on a local socket for controllers (nbtctl and the iDrive plugin)
// and installs what they ask for and offer.
//
//   nbtagent --root DIR [--socket PATH]      on a folder that stands in for the head unit
//   nbtagent                                 on the head unit itself: in the build for the head unit only
//   (host build also: [--test-key FILE]... [--debug-tcp [ADDRESS:]PORT])
//   (head unit's build also: [--bluetooth] [--no-bluetooth] [--decide MS] [--decide-hold], and
//    `nbtagent --look PATH` for its own use)
//
// With --root, everything it touches is inside DIR, laid out as host_platform.hpp says: installed records,
// staging, ${app}, ${data}, ${plugins}, and absolute destinations under DIR/system. It remounts nothing and
// restarts nothing; it counts those requests and carries on. That works on a car as well as on a build host, and
// is the way to try the agent on one without changing it.
//
// Without --root, the build for the head unit (qcc, __QNXNTO__) works on the head unit: the real folders, the
// system partition remounted for a write and put back, the HMI restarted (hu_platform.hpp). What it needs to know
// for that is HeadUnitConfig::car(). On a machine that lacks the system partition's folder or the plugin folder
// it ends, having made nothing. In this mode:
//   - the socket is /tmp/nbtagent.sock and cannot be put elsewhere. The socket is what keeps a second agent off
//     the same head unit: one that finds an agent answering there ends before it looks at anything else;
//   - SIGHUP is ignored, so the agent outlives the SSH session it was started from;
//   - an agent that finds the system partition left writable by one that died puts it back, and says so.
// The host's build has none of this code. With no --root it prints its usage.
//
// It trusts the package key in embedded_keys.hpp, the official source's, and the head unit's build no other. A
// build with NBT_TEST_KEYS (the host tests') also takes --test-key FILE, which trusts a key file made by
// `nbtpkg.py keygen` as if it were built in. Test keys' private halves are public: such a build is for the test
// bench only.
//
// A build with NBT_DEBUG_TCP (again the host's) also takes --debug-tcp PORT: a second listener, on TCP at
// 127.0.0.1, for the debug build of the phone app on an emulator or a phone on USB (which reach it after
// `adb reverse tcp:PORT tcp:PORT`). A phone on it has to be paired and to do `auth`, as on Bluetooth serial
// (`nbtctl pair` opens the pairing window and prints the code), but the link itself is not protected: a pairing
// key crosses it in the clear and its frames carry no MAC, so whatever can read or change the bytes on that
// port's connections can act as a paired phone, and so install a package signed by a trusted key; with a test
// key that is any package at all, hooks included. Test bench only, like the test keys. The head unit's build has
// neither option, and does not compile with them.
//
// A hook is an ordinary process of whoever started the agent: --root says where the agent puts files, and does
// not confine a script. So on the bench the agent runs in a Docker container that sees the root folder and
// little else: tools/bench.py starts it there. In a container 127.0.0.1 is the container's own, and a port
// that Docker publishes arrives on another address, so --debug-tcp also takes ADDRESS:PORT; an address other
// than 127.0.0.1 is refused outside a container, where it would open the link to the network.
//
// It writes what it does to standard error, a line at a time with the time of day: the links that come and go,
// each request and its answer, plans, what it fetches, and every step, phase and log line of a job, hooks'
// output included (Agent::setLog in agent.hpp). On a head unit that is all the log there is: the launcher
// (nbtlaunch.sh) sends it to a file on the RAM disk and names the file in NBTAGENT_LOG. An agent that finds its
// standard error to be that file keeps it short, because that disk is small: when it has passed 512 KB
// the file becomes agent.log.1, over the one before, and the log goes on in a new one (capLog in agent.hpp).
//
// The exit status says why it ended (exit_status.hpp): the launcher starts an agent again that fell over, and not
// one that was stopped, was started wrongly, or found another agent at work.
//
// It updates itself with its own package, org.nbtapp.agent, where it was put for a launcher to start: a folder
// that holds one folder to a version and a file `current` that names this one (agent_home.hpp), which is
// /var/opt/nbtapp/agent on a head unit and DIR/agent with --root. When a job has placed a newer version there,
// the agent ends with status 4 and the launcher starts that version, on trial: see agent.hpp. Anywhere else the
// package is refused, because nothing would start the new version.
//
// Bluetooth serial (serial_link.hpp), in the build for the head unit only: phone apps connect to the agent through
// the car's Bluetooth stack. The agent registers a serial service with the stack at run time and puts a gate in
// front of /dev/btspp0 that refuses the car's media process, and takes both away again when it stops; nothing of
// it is written to disk. While the agent runs with it, the car maker's phone apps do not work over Bluetooth. It
// is on without --root, where --no-bluetooth turns it off, and off with --root, where --bluetooth turns it on: a
// folder keeps the agent's files off the head unit, and the gate and the service are on the head unit all the
// same. If it cannot be set up the agent says why and goes on with the local socket. The phone's Bluetooth
// pairing with the car lets any app on that phone open the link, so the agent asks more: a phone app is paired
// with the agent itself, by a code from the car's screen, and proves that with `auth` on every link (agent.hpp).
// An agent that ends without stopping leaves its registration behind until the car's Bluetooth restarts or the
// next agent replaces it, so with --root, where SIGHUP is not ignored, SIGHUP stops the agent as SIGTERM does.
// Two options are for finding out what a car needs of the decide step, which lets go the links of other programs
// (serial_link.hpp): --decide MS waits that long for a link's magic bytes in place of the protocol's
// serial.decideMs, and --decide-hold keeps the node of a link that is let go open, unread, until the link ends,
// in place of closing it. The agent's first lines in the log say which of the two it goes by, and which programs
// the gate holds off a link meanwhile.
// `nbtagent --look PATH` is how the gate is checked, by this program run again: one stat() of PATH, answered by
// the exit code.
//
// SIGINT and SIGTERM stop it between install steps, never inside one. An agent that stops, or ends for a newer
// version of itself, tells each phone so first (link.closing, agent.hpp) and closes its links; with Bluetooth
// serial it then waits, five seconds at most, for the phones to end their links, before it takes its service and
// its gate away and ends (serial_link.hpp). `kill -9` leaves it no time for any of that.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "agent.hpp"
#include "agent_home.hpp"
#include "embedded_keys.hpp"
#include "exit_status.hpp"
#include "fsutil.hpp"
#include "hex.hpp"
#include "host_platform.hpp"
#include "version.hpp"

#ifndef NBT_APP_VERSION
#define NBT_APP_VERSION "unknown"
#endif

// The mode that works on a head unit is compiled for QNX only: no build for another machine holds code that would
// take that machine's own /var/opt and /opt for a car's.
#ifdef __QNXNTO__
#define NBT_HEAD_UNIT 1
#include <errno.h>
#include <sys/stat.h>

#include "hu_platform.hpp"
#include "serial_link.hpp"
#if defined(NBT_TEST_KEYS) || defined(NBT_DEBUG_TCP) || defined(NBT_TEST_AGENT_VERSION)
#error "the head unit's nbtagent takes no test key, has no debug TCP link and says its own version: those are for the test bench"
#endif
#endif

using namespace nbt;

static volatile sig_atomic_t gStop = 0;

static void onStop(int) { gStop = 1; }

static int usage() {
    fprintf(stderr,
#ifdef NBT_HEAD_UNIT
            "usage: nbtagent [--no-bluetooth] [--decide MS] [--decide-hold]\n"
            "                                             work on this head unit\n"
            "       nbtagent --root DIR [--socket PATH] [--bluetooth [--decide MS] [--decide-hold]]\n"
            "                                             work on a folder that stands in for one\n"
            "  (no --root)     state in /var/opt/nbtapp, the socket at %s; the system\n"
            "                  partition is remounted for a write and put back, the HMI restarted\n"
            "  --bluetooth     also take phone apps' links over Bluetooth serial: the default without\n"
            "                  --root. While the agent runs with it, the car maker's phone apps do not\n"
            "                  work over Bluetooth\n"
            "  --no-bluetooth  the local socket only: the default with --root\n"
            "  --decide MS     with Bluetooth serial: how long a link's magic bytes are waited for, from\n"
            "                  the open of its node, before the link is let go as another program's\n"
            "                  (1 to %u; default %u, the protocol's)\n"
            "  --decide-hold   with Bluetooth serial: a link that is let go keeps its node open, unread,\n"
            "                  until the link ends, in place of having it closed\n"
#else
            "usage: nbtagent --root DIR [--socket PATH]"
#ifdef NBT_TEST_KEYS
            " [--test-key FILE]..."
#endif
#ifdef NBT_DEBUG_TCP
            " [--debug-tcp [ADDRESS:]PORT]"
#endif
            "\n"
#endif
            "  --root DIR      keep everything under DIR: state, staging and what packages install\n"
            "  --socket PATH   where controllers connect (default: DIR/agent.sock)\n"
#ifdef NBT_TEST_KEYS
            "  --test-key FILE trust the package key in FILE as a built-in key. TEST BENCH ONLY\n"
#endif
#ifdef NBT_DEBUG_TCP
            "  --debug-tcp PORT also listen on TCP at 127.0.0.1:PORT (0: any free port). A phone is\n"
            "                  paired over it, in the clear. TEST BENCH ONLY. ADDRESS:PORT listens at\n"
            "                  another IPv4 address, and only inside a Docker container\n"
#endif
            "  --version\n"
#ifdef NBT_HEAD_UNIT
            ,
            kHeadUnitSocket, kSerialDecideMostMs, (unsigned)kSerialDecideMs
#endif
            );
    return kExitUsage;
}

#ifdef NBT_TEST_KEYS
// A public key file as `nbtpkg.py keygen` writes it: comment lines, then 64 hex characters.
static bool readKeyFile(const std::string& path, TrustedKey* out) {
    std::string text;
    if (!fs::readFile(path, 4096, &text, 0)) return false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ')) line.erase(line.size() - 1);
        if (line.empty() || line[0] == '#') continue;
        return fromHex(line, out->key, sizeof out->key);
    }
    return false;
}
#endif

struct Options {
    std::string root, socketPath;
    std::vector<TrustedKey> keys;
    long debugTcpPort;             // below 0: no TCP link. Only a build with NBT_DEBUG_TCP sets it
    std::string debugTcpAddress;
    int bluetooth;                 // 1: Bluetooth serial, 0: none, below 0: not said. Only the head unit's build
    unsigned decideMs;             // --decide; 0: not given, so the protocol's time. Only the head unit's build
    bool decideHold;               // --decide-hold. Only the head unit's build
    std::string self;              // how this program was started, to start it again for the gate's check

    Options() : debugTcpPort(-1), debugTcpAddress("127.0.0.1"), bluetooth(-1), decideMs(0), decideHold(false) {}
};

#ifdef NBT_HEAD_UNIT
// Set once Bluetooth serial has been tried: the car's service library may be loaded then, and its thread runs on
// until the process ends.
static bool gBluetoothTried = false;

class BluetoothLog : public SerialLog {
public:
    virtual void line(const std::string& line) { logLine(stderr, "bluetooth: " + line); }
};

static void serviceClientLine(const char* line, void*) { logLine(stderr, std::string("bluetooth: ") + line); }

// Everything Bluetooth serial is made of. Made once and never freed: the car's service library goes on calling
// into its client for as long as the process lives.
struct Bluetooth {
    BluetoothLog log;
    btspp::DsiGapClient gap;
    SppNodeGate gate;
    SerialListener listener;
    SerialServices services;   // what other programs register, for the removal of what one leaves behind

    Bluetooth(const std::string& self, const Platform* platform, const Store* store, const SerialConfig& config)
        : gate(self, &log), listener(&gap, &gate, &log, config), services(platform, store, &log) {
        gap.setLog(&serviceClientLine, 0);
    }
};

// The listener's settings: the built-in ones, with what --decide and --decide-hold change.
static SerialConfig serialConfigOf(const Options& options) {
    SerialConfig config;
    if (options.decideMs != 0) config.decideMs = options.decideMs;
    config.letGoKeepsNode = options.decideHold;
    return config;
}

// Waits, `ms` at most, until every serial link whose agent's end has closed has been ended by its phone, or has
// had its time for that (serial_link.hpp). Each link says in the log which of the two it was.
static void waitForPhones(int ms) {
    unsigned open = serialLinksOpen();
    if (open == 0) return;
    char text[160];
    snprintf(text, sizeof text, "bluetooth: %u serial link%s closed by the agent and not yet ended by the phone: waiting, %d ms at most",
             open, open == 1 ? " is" : "s are", ms);
    logLine(stderr, text);
    if (!waitForSerialLinks(ms)) logLine(stderr, "bluetooth: a serial link is still up, and ends with this process");
}
#endif

// Opens the socket: 0, or the exit status to end with. Both modes do this before they look at any state: an agent
// that is already at work there answers on the socket, and this one then ends without having touched a journal
// that is not its own to recover. Starting it again would find the same, and its status says so.
static int listenAt(const std::string& path, LocalListener* listener) {
    std::string why;
    bool taken = false;
    if (listener->open(path, &why, &taken)) return 0;
    fprintf(stderr, "nbtagent: %s\n", why.c_str());
    return taken ? kExitNoRestart : kExitFailed;
}

// The starts with the head unit that the iDrive plugin has on record (src/agent_start.hpp) were not ones that
// brought the head unit down: this agent has run for long enough, or was stopped by somebody. Writes nothing
// where there is no record, which is every start that the plugin did not make.
static void forgetAutostarts(Platform* platform, const char* why) {
    if (AgentHome(platform->agentHome()).forgetAutostarts()) {
        logLine(stderr, std::string("the starts with the head unit on record in ") + kAutostartBoots + " are forgotten: " + why);
    }
}

// The agent on `platform`, until it is told to stop. `where` goes into the first log line, `note` after it.
static int serve(Platform* platform, LocalListener* listener, const std::string& where, const std::string& note,
                 const Options& options) {
    std::string why;
    SystemClock clock;
    SystemRandom random;
    Agent agent(platform, options.keys, kAgentVersion, &clock, &random);
    if (!agent.init(&why)) {
        fprintf(stderr, "nbtagent: cannot start: %s\n", why.c_str());
        return kExitFailed;
    }
    agent.addListener(listener);
    logLine(stderr, std::string(kAgentVersion) + " (build " NBT_APP_VERSION "), head unit " + agent.headUnitId() + ", " + where +
                        ", listening at " + options.socketPath);
    if (!note.empty()) logLine(stderr, note);
    {
        std::vector<InstalledPackage> installed;
        if (agent.engine().store().loadAll(&installed, &why)) {
            std::string line;
            for (size_t i = 0; i < installed.size(); ++i) {
                line += (i == 0 ? ": " : ", ") + installed[i].manifest.id + " " + installed[i].manifest.version;
            }
            char count[32];
            snprintf(count, sizeof count, "%u", (unsigned)installed.size());
            logLine(stderr, std::string(count) + (installed.size() == 1 ? " package installed" : " packages installed") + line);
        }
    }

#ifdef NBT_DEBUG_TCP
    TcpListener tcp;
    if (options.debugTcpPort >= 0) {
        if (!tcp.open(options.debugTcpAddress.c_str(), (unsigned)options.debugTcpPort, &why)) {
            fprintf(stderr, "nbtagent: %s\n", why.c_str());
            return kExitFailed;
        }
        agent.addListener(&tcp);
        char port[16];
        snprintf(port, sizeof port, "%u", tcp.port());
        logLine(stderr, "DEBUG TCP link at " + options.debugTcpAddress + ":" + port + ", over which a phone is paired in the clear. This agent is for the test bench only.");
    }
#endif

#ifdef NBT_HEAD_UNIT
    Bluetooth* bluetooth = 0;
    if (options.bluetooth > 0) {
        gBluetoothTried = true;
        const SerialConfig config = serialConfigOf(options);
        bluetooth = new Bluetooth(options.self, platform, &agent.engine().store(), config);
        // The services that other programs register are known before the listener's thread first asks for them,
        // which is when its own service has been registered.
        bluetooth->services.reload();
        std::vector<SerialServiceOwner> declared = bluetooth->services.declared();
        for (size_t i = 0; i < declared.size(); ++i) {
            logLine(stderr, "bluetooth: serial service '" + declared[i].name + "' is the program " + declared[i].program +
                                "'s, says " + declared[i].from);
        }
        bluetooth->services.setRemover(&bluetooth->listener);
        bluetooth->listener.setServices(&bluetooth->services);
        if (bluetooth->listener.start(&why)) {
            agent.addListener(&bluetooth->listener);
            agent.engine().setStepObserver(&bluetooth->services);
            logLine(stderr, std::string("Bluetooth serial: service '") + kSerialServiceName + "', UUID " + kSerialServiceUuid +
                                ". While this agent runs, the car maker's phone apps do not work over Bluetooth.");
            // What a trial on a car is read against: the decide step as this agent runs it.
            logLine(stderr, "bluetooth: " + describeSerialDecide(config));
            logLine(stderr, "bluetooth: " + describeSerialLinger(config));
            logLine(stderr, "bluetooth: " + describeSerialRetry(config));
            logLine(stderr, "bluetooth: " + describeSerialHold(bluetooth->services.programs()));
        } else {
            bluetooth->services.setRemover(0);
            logLine(stderr, "no Bluetooth serial link: " + why);
        }
    }
#endif

    if (agent.onTrial()) logLine(stderr, "ON TRIAL: this version stays only once a controller has asked for the job's status");
    bool saidStopping = false, handOver = false, settled = false;
    const int64_t startedMs = clock.nowMs();
    while (!gStop || agent.busy()) {
        if (gStop && !saidStopping) {
            logLine(stderr, "asked to stop; finishing the install step first");
            saidStopping = true;
        }
        agent.turn(500);
        if (!settled && clock.nowMs() - startedMs >= (int64_t)kAutostartSettledS * 1000) {
            settled = true;
            forgetAutostarts(platform, "the agent has run for five minutes");
        }
        // A newer version of this agent is in place and its controllers know: the launcher starts it when this
        // process has ended. An agent that was asked to stop in the meantime stops, and the launcher with it;
        // the new version then has its try when a launcher is next started.
        if (!gStop && agent.restartDue()) {
            handOver = true;
            break;
        }
    }
    // The phones are told that the agent is going, and why, and their links are closed here. A phone app ends its
    // link when it hears that, and connects again if a newer version is about to start.
    agent.endPhoneLinks(handOver ? kClosingRestarting : kClosingStopping, 2000);
#ifdef NBT_HEAD_UNIT
    if (bluetooth != 0) {
        // The process stays for as long as the serial links wait for their phones to end them: a node that goes
        // under a phone which still holds the link is what the car's Bluetooth did not get over. The registration
        // and the gate stay for that long too, as they were while the links were up.
        const int wait = (int)serialConfigOf(options).lingerMs + 1000;
        waitForPhones(wait);
        // Takes the service's registration and the gate away again.
        agent.engine().setStepObserver(0);
        bluetooth->services.setRemover(0);
        bluetooth->listener.stop();
        // A link that a phone opened while the agent was going was ended by stop(), with nobody to serve it.
        waitForPhones(wait);
    }
#endif
    if (handOver) {
        logLine(stderr, "a newer version of the agent is placed; ending, for the launcher to start it");
        return kExitRestart;
    }
    forgetAutostarts(platform, "the agent was stopped");
    logLine(stderr, "stopped");
    return kExitStopped;
}

// On a folder that stands in for the head unit.
static int onFolder(Options options) {
    if (options.root[0] != '/') {
        // Hooks run in a folder of their own and are handed paths under the root, so it has to be absolute.
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd) == 0) {
            fprintf(stderr, "nbtagent: cannot tell which folder this is\n");
            return kExitFailed;
        }
        options.root = std::string(cwd) + "/" + options.root;
    }
    if (options.socketPath.empty()) options.socketPath = options.root + "/agent.sock";
    std::string why;
    if (!fs::makeDirs(options.root, &why)) {
        fprintf(stderr, "nbtagent: %s\n", why.c_str());
        return kExitFailed;
    }
    LocalListener listener;
    int refused = listenAt(options.socketPath, &listener);
    if (refused != 0) return refused;
    HostPlatform platform(options.root);
    if (!platform.prepare(&why)) {
        fprintf(stderr, "nbtagent: %s\n", why.c_str());
        return kExitFailed;
    }
    return serve(&platform, &listener, "root " + options.root, "", options);
}

#ifdef NBT_HEAD_UNIT
// On the head unit itself.
static int onHeadUnit(Options options) {
    HeadUnitConfig config = HeadUnitConfig::car();
    std::string missing = config.missing();
    if (!missing.empty()) {
        fprintf(stderr,
                "nbtagent: this build does not know enough about the head unit to work on it; not set: %s\n"
                "nbtagent: hu_probe.sh reads those off a car, and HeadUnitConfig::car() (agent/hu_platform.cpp) is where\n"
                "nbtagent: they go. Nothing was touched. `nbtagent --root DIR` works on a folder instead.\n",
                missing.c_str());
        return kExitNoRestart;
    }
    // Before prepare(): with an agent at work here, prepare() would take that agent's marker for one left by an
    // agent that died, and put the system partition back under its hands.
    options.socketPath = kHeadUnitSocket;
    LocalListener listener;
    int refused = listenAt(options.socketPath, &listener);
    if (refused != 0) return refused;
    // From here on the agent is meant to outlive the SSH session it was started from.
    signal(SIGHUP, SIG_IGN);
    std::string why;
    HeadUnitPlatform platform(config);
    if (!platform.prepare(&why)) {
        fprintf(stderr, "nbtagent: %s\n", why.c_str());
        return kExitFailed;
    }
    return serve(&platform, &listener,
                 "on this head unit (state " + config.stateDir + ", staging " + config.stagingDir + ", system partition " +
                     config.systemMount + ")",
                 platform.startNote(), options);
}
#endif

int main(int argc, char** argv) {
#ifdef NBT_HEAD_UNIT
    // The gate's check (SppNodeGate): this program run again, as any other program's look at the path.
    if (argc == 3 && strcmp(argv[1], "--look") == 0) {
        struct stat st;
        return btspp::lookExitCode(stat(argv[2], &st) == 0 ? 0 : errno);
    }
#endif
    Options options;
    options.self = argv[0];
    addEmbeddedKeys(&options.keys);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : 0;
        if (arg == "--version") {
            printf("nbtagent %s (build %s)\n", kAgentVersion, NBT_APP_VERSION);
            return 0;
        } else if (arg == "--root" && value != 0 && *value != 0) {
            options.root = value;
            ++i;
        } else if (arg == "--socket" && value != 0 && *value != 0) {
            options.socketPath = value;
            ++i;
#ifdef NBT_HEAD_UNIT
        } else if (arg == "--bluetooth") {
            options.bluetooth = 1;
        } else if (arg == "--no-bluetooth") {
            options.bluetooth = 0;
        } else if (arg == "--decide" && value != 0) {
            if (!parseSerialDecideMs(value, &options.decideMs)) {
                fprintf(stderr, "nbtagent: --decide takes milliseconds from 1 to %u\n", kSerialDecideMostMs);
                return kExitUsage;
            }
            ++i;
        } else if (arg == "--decide-hold") {
            options.decideHold = true;
#endif
#ifdef NBT_TEST_KEYS
        } else if (arg == "--test-key" && value != 0) {
            TrustedKey key;
            if (!readKeyFile(value, &key)) {
                fprintf(stderr, "nbtagent: %s is not a public key file\n", value);
                return kExitUsage;
            }
            key.name = "test key";
            key.embedded = true;
            options.keys.push_back(key);
            logLine(stderr, std::string("TEST KEY ") + value + " is trusted. This agent is for the test bench only.");
            ++i;
#endif
#ifdef NBT_DEBUG_TCP
        } else if (arg == "--debug-tcp" && value != 0) {
            const char* port = strrchr(value, ':');
            if (port != 0) {
                options.debugTcpAddress.assign(value, port - value);
                ++port;
            } else {
                port = value;
            }
            char* end = 0;
            options.debugTcpPort = strtol(port, &end, 10);
            if (*port == 0 || *end != 0 || options.debugTcpPort < 0 || options.debugTcpPort > 65535) return usage();
            ++i;
#endif
        } else {
            return usage();
        }
    }
#ifdef NBT_HEAD_UNIT
    // On the head unit the socket's place is fixed: see the top of this file.
    if (options.root.empty() && !options.socketPath.empty()) return usage();
    if (options.bluetooth < 0) options.bluetooth = options.root.empty() ? 1 : 0;
    if (options.bluetooth == 0 && (options.decideMs != 0 || options.decideHold)) {
        fprintf(stderr, "nbtagent: --decide and --decide-hold are about Bluetooth serial, which is off%s\n",
                options.root.empty() ? "" : ": with --root it takes --bluetooth");
        return kExitUsage;
    }
#else
    if (options.root.empty()) return usage();
#endif
#ifdef NBT_DEBUG_TCP
    // Docker puts this file into every container it starts. Outside one, any other address than the loopback
    // would let the network install packages here.
    if (options.debugTcpPort >= 0 && options.debugTcpAddress != "127.0.0.1" && access("/.dockerenv", F_OK) != 0) {
        fprintf(stderr, "nbtagent: --debug-tcp takes an address other than 127.0.0.1 only inside a Docker container: "
                        "the link is not protected\n");
        return kExitFailed;
    }
#endif

    // A controller that goes away mid-write must not take the agent with it.
    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = onStop;
    sigaction(SIGINT, &action, 0);
    sigaction(SIGTERM, &action, 0);

    // The launcher's file for what this program says. A hook has no use for the name.
    const char* logPath = getenv(kLogPathVariable);
    if (logPath != 0 && *logPath != 0) capLog(logPath, kLogCapBytes);
    unsetenv(kLogPathVariable);

#ifdef NBT_HEAD_UNIT
    // A dropped SSH session must still take the registration away. Without --root, SIGHUP is ignored instead.
    if (options.bluetooth > 0 && !options.root.empty()) sigaction(SIGHUP, &action, 0);
    int rc = options.root.empty() ? onHeadUnit(options) : onFolder(options);
    if (gBluetoothTried) {
        // The links' threads have closed their nodes and said so: serve() waited for them. This is for an agent
        // that ended some other way.
        waitForSerialLinks(1000);
        // The service library's thread is still running: no static destructors under it.
        fflush(stdout);
        fflush(stderr);
        _exit(rc);
    }
    return rc;
#else
    return onFolder(options);
#endif
}

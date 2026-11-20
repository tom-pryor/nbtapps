// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// sshrelay: the program of the SSH package's channel (relay.hpp). The agent starts it, in ${app}, with a hook's
// environment and NBT_CHANNEL, its standard error appended to the channel's log; it takes no arguments.
//
//   NBT_TMP_DIR    the package's folder on the RAM disk: the socket is channels/$NBT_CHANNEL in it, the keys folders
//                  are in auth/, the state file is sshrelay.state
//   NBT_DATA_DIR   the package's data folder: hostkey_ed25519
//   NBT_APP_DIR    the package's folder: bin/dropbearmulti (the server and its key tool), bin/sftp-server, and scp
//                  as a link to dropbearmulti
//   NBT_CHANNEL    the channel's name
//
// Exits 0 after SIGTERM, SIGINT or SIGHUP, and 2 when it cannot start: a variable missing, or no socket.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "relay.hpp"

namespace {

bool fromEnv(const char* name, std::string* out) {
    const char* value = getenv(name);
    if (value == 0 || value[0] == '\0') {
        sshrelay::logLine(std::string("cannot start: ") + name + " is not set");
        return false;
    }
    *out = value;
    return true;
}

void handle(int signal, void (*handler)(int)) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    // No SA_RESTART: select returns at a signal.
    sigaction(signal, &action, 0);
}

}  // namespace

int main(int argc, char** argv) {
    (void)argv;
    if (argc > 1) {
        sshrelay::logLine("cannot start: sshrelay takes no arguments");
        return 2;
    }
    // Standard input, output and error are open, so that no socket of the relay's is ever one of them.
    for (int fd = 0; fd < 3; ++fd) {
        if (fcntl(fd, F_GETFD) < 0) {
            int null = open("/dev/null", O_RDWR);
            if (null >= 0 && null != fd) close(null);
        }
    }
    std::string tmp, data, app, channel;
    if (!fromEnv("NBT_TMP_DIR", &tmp) || !fromEnv("NBT_DATA_DIR", &data) || !fromEnv("NBT_APP_DIR", &app) ||
        !fromEnv("NBT_CHANNEL", &channel)) {
        return 2;
    }
    if (channel.find('/') != std::string::npos || channel == "." || channel == "..") {
        sshrelay::logLine("cannot start: NBT_CHANNEL is not a name");
        return 2;
    }

    sshrelay::Config config;
    config.socketPath = tmp + "/channels/" + channel;
    config.authDir = tmp + "/auth";
    config.statePath = tmp + "/sshrelay.state";
    config.hostKeyPath = data + "/hostkey_ed25519";
    config.multiPath = app + "/bin/dropbearmulti";
#ifdef __QNXNTO__
    // The car's debug set-up's pty driver, started on demand (relay.hpp, "The pty driver").
    config.driverPath = "/bin/devc-pty";
    config.ptyDevDir = "/dev";
#endif
#ifdef NBT_SSHRELAY_TEST_HOOK
    // Tests only; the head unit's build has none of these variables.
    //   NBT_SSHRELAY_TEST_SERVER       a program that takes the server's place, given the server's arguments after
    //                                  `dropbear`
    //   NBT_SSHRELAY_TEST_SERVER_NAME  the program name that program's processes have (a script's is its
    //                                  interpreter's), for what a start checks before it ends a recorded server
    //   NBT_SSHRELAY_TEST_PTY_DRIVER   a stand-in for /bin/devc-pty, treated as that is on QNX
    //   NBT_SSHRELAY_TEST_DEV          a folder that stands for QNX's /dev where pty names are looked for; without
    //                                  it there are always ptys
    //   NBT_SSHRELAY_TEST_DRIVER_MS    the driver's 30 s (with no connection, and between failed starts)
    const char* value = getenv("NBT_SSHRELAY_TEST_SERVER");
    if (value != 0 && value[0] != '\0') config.testServer = value;
    value = getenv("NBT_SSHRELAY_TEST_SERVER_NAME");
    if (value != 0 && value[0] != '\0') config.testServerName = value;
    value = getenv("NBT_SSHRELAY_TEST_PTY_DRIVER");
    if (value != 0 && value[0] != '\0') config.driverPath = value;
    value = getenv("NBT_SSHRELAY_TEST_DEV");
    if (value != 0 && value[0] != '\0') config.ptyDevDir = value;
    value = getenv("NBT_SSHRELAY_TEST_DRIVER_MS");
    if (value != 0 && atol(value) > 0) config.driverIdleMs = config.driverRetryMs = atol(value);
#endif
    // The environment of the server, and through -e of what it runs without a pty: the package's programs first
    // (sftp-server, scp), then the head unit's own folders as its shell has them. A login shell reads
    // /etc/profile, which sets its own.
#ifdef __QNXNTO__
    config.env.push_back("PATH=" + app + "/bin:/proc/boot:/boot/bin:/bin:/opt/os/bin:/opt/sys/bin:/opt/core/bin:/opt/car/bin");
    config.env.push_back("LD_LIBRARY_PATH=/proc/boot:/boot/lib:/lib:/usr/lib:/opt/os/lib:/opt/sys/lib:/opt/core/lib:/opt/car/lib");
#else
    const char* path = getenv("PATH");
    config.env.push_back("PATH=" + app + "/bin:" + (path != 0 && path[0] != '\0' ? path : "/usr/bin:/bin"));
#endif

    signal(SIGPIPE, SIG_IGN);
    handle(SIGTERM, sshrelay::Relay::onSignal);
    handle(SIGINT, sshrelay::Relay::onSignal);
    handle(SIGHUP, sshrelay::Relay::onSignal);
    handle(SIGCHLD, sshrelay::Relay::onSignal);

    sshrelay::Relay relay(config);
    return relay.run();
}

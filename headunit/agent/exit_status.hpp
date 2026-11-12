// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_EXIT_STATUS_HPP
#define NBT_AGENT_EXIT_STATUS_HPP

// nbtagent's exit statuses. A version's launcher (agent/nbtlaunch.sh) starts the agent, and starts it again when it
// fails; the status is all it has to tell an agent that was stopped from one that fell over. It ends with 0 and
// 4 itself when the agent did, which is what the starter (agent/nbtstart.sh) goes by: on 4 it looks again at
// which version to start. Both scripts hold the numbers they use under the same names in capitals, and
// tests/agent/test_exit_status.cpp fails when they differ from these.
//
// A status that is not named here, or an end by a signal, is a failure like kExitFailed.

namespace nbt {

const int kExitStopped = 0;     // asked to stop (SIGINT, SIGTERM): the launcher ends too
const int kExitFailed = 1;      // could not start, or could not go on: the launcher tries again, a few times
const int kExitUsage = 2;       // started with arguments it does not take: the launcher ends
const int kExitNoRestart = 3;   // nothing a new start would change: another agent has the socket, or this build
                                // does not know the head unit it is on. The launcher ends
const int kExitRestart = 4;     // wants to be started again, at once: not a failure. An agent that has placed a
                                // newer version of itself ends with it, and the launcher starts that version

}  // namespace nbt

#endif

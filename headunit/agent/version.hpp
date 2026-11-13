// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_VERSION_HPP
#define NBT_AGENT_VERSION_HPP

// The agent's version: what `hello` and info.get report, and what a manifest's requires.agent is checked against.
// It is a semver of its own, raised when the agent learns something a package can depend on; the build's git
// description is a different thing and is only printed.

namespace nbt {

#ifdef NBT_TEST_AGENT_VERSION
// The host tests' second build of nbtagent: the same program, saying that it is a later version, for an update
// of the agent by the agent to be run from end to end. nbtagent.cpp does not compile for a head unit with it.
const char kAgentVersion[] = NBT_TEST_AGENT_VERSION;
#else
const char kAgentVersion[] = "1.3.0";
#endif

}  // namespace nbt

#endif

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_SSHRELAY_VERSION_HPP
#define NBT_SSHRELAY_VERSION_HPP

// The SSH package's version, org.nbtapp.ssh: what tools/ssh_pkg.py makes the package as, and what the relay says it
// is (sshrelay --version, and the first line of its log), with the build's git description beside it. It is the
// package's own, not the agent's, and is raised whenever any of the package's programs changes: a version that has
// been released is never built again with other content.
#define NBT_SSH_VERSION "1.0.0-beta.1"

#ifndef NBT_APP_VERSION
#define NBT_APP_VERSION "unknown"
#endif

// What the relay says it is. tools/ssh_pkg.py and tools/check_ssh.py read it out of the program's bytes.
#define NBT_SSHRELAY_BUILD_LINE "sshrelay " NBT_SSH_VERSION " (build " NBT_APP_VERSION ")"

#endif

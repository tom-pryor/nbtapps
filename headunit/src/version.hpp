// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBTAPP_VERSION_HPP
#define NBTAPP_VERSION_HPP

// The iDrive plugin's version: the version its package, org.nbtapp.plugin, is released as. tools/plugin_pkg.py
// reads the line below, and a version that has been released is never built again with other content: raise it.
//
// It is the plugin's own. The agent has another, in agent/version.hpp, and the two are released apart: the
// plugin's package names the oldest agent it runs with (plugin/package/manifest.json.in, requires.agent), and
// an agent runs with any plugin that is older than it would like, without what the newer one brings.
#define NBT_PLUGIN_VERSION "1.1.1"

#endif

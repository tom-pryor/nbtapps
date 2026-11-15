// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_HMI_IDS_HPP
#define NBT_AGENT_HMI_IDS_HPP

// The two ids of an iDrive PluginApp that must be unique across every app on the car: the connector id in its
// <Name>.manifest, and the entryButtonFocusID of each pluginApp in its UI description. A package's own manifest
// does not repeat them; they are read from the plugin's files, the new package's and the ones already in Plugins.
//
//   <plugin> ... <pluginAppConnector> <id>211</id> ... <DescFile>{pluginDir}/NbtApp.xml</DescFile>
//   <pluginApps> <pluginApp entryButtonFocusID="32754" ...>
//
// The reader is a small scanner, not an XML parser: it follows elements, attributes, comments and CDATA, and takes
// what it recognises. A file it cannot make sense of yields no ids rather than an error, because the files already
// on a car are not ours to judge.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>

#include <set>
#include <string>
#include <vector>

namespace nbt {

// From the text of a <Name>.manifest: every pluginAppConnector id, and the DescFile parameter (empty if none).
void readPluginManifest(const std::string& xml, std::vector<long>* connectorIds, std::string* descFile);

// From the text of a UI description: the entryButtonFocusID of every pluginApp.
void readPluginDescription(const std::string& xml, std::vector<long>* focusIds);

// Connector ids of the apps built into the HMI, which are in use on every car.
bool builtInConnectorId(long id);

// A plugin manifest or UI description larger than this is not read.
const size_t kMaxPluginFile = 4u << 20;

// One <x>.manifest in a plugin folder: the connector ids it claims, and the focus ids of the UI description it
// names with {pluginDir}/.
struct PluginManifest {
    std::string path;
    std::vector<long> connectorIds, focusIds;
};

// A folder in one of the folders the HMI loads plugins from, and the manifests in it, sorted by name.
struct PluginFolder {
    std::string dir;     // <scan folder>/<name>
    std::string name;
    std::vector<PluginManifest> manifests;
};

// Every folder directly in one of `scanDirs`, or link that leads to one, with the ids its manifests claim. A
// manifest whose path is in `skip` is passed over, and a UI description whose path is in it is not read: an install
// step names there the files of the package it places, which are not another app's. A folder that cannot be
// listed, and a file that cannot be read, are passed over too. It reads files and changes nothing.
void scanPluginFolders(const std::vector<std::string>& scanDirs, const std::set<std::string>& skip,
                       std::vector<PluginFolder>* out);

}  // namespace nbt

#endif

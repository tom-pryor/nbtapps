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

#include <string>
#include <vector>

namespace nbt {

// From the text of a <Name>.manifest: every pluginAppConnector id, and the DescFile parameter (empty if none).
void readPluginManifest(const std::string& xml, std::vector<long>* connectorIds, std::string* descFile);

// From the text of a UI description: the entryButtonFocusID of every pluginApp.
void readPluginDescription(const std::string& xml, std::vector<long>* focusIds);

// Connector ids of the apps built into the HMI, which are in use on every car.
bool builtInConnectorId(long id);

}  // namespace nbt

#endif

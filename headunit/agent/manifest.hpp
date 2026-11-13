// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_MANIFEST_HPP
#define NBT_AGENT_MANIFEST_HPP

// manifest.json of a package, checked and turned into plain structs. The checks are the ones tools/nbtpkg.py
// makes, in the same order, so both give the same code for the same file:
//
//   1. at most 1 MiB, and inside the strict JSON profile                         bad-manifest
//   2. manifestVersion is an integer (bad-manifest) that this agent knows        unsupported-manifest
//   3. spec/manifest.schema.json, walked in the order of the text: a wrong type, form or range is bad-manifest;
//      a key the schema does not know, inside requires or install, is            unsupported-manifest
//   4. the cross-field rules: unique paths, every src and hook script listed in contents, a folder to a folder,
//      no destination placed twice, no serial service declared twice, no log path named twice
//                                                                                  bad-manifest
//
// The schema is written out by hand in manifest.cpp, because the agent has no regular expressions. The test
// packages and tests/agent/test_manifest.cpp hold the two implementations together.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "error.hpp"

namespace nbt {

const size_t kMaxManifestBytes = 1u << 20;
const size_t kMaxPathBytes = 255;
const int kManifestVersion = 1;   // the highest manifestVersion this agent installs
const size_t kMaxLogs = 16;       // entries in a manifest's logs

struct ManifestDependency {
    std::string id;
    std::string range;   // empty: any version
};

struct ManifestContent {
    std::string path;
    uint64_t size;
    std::string sha256;
};

struct ManifestFile {
    std::string src, dest;
    unsigned mode;
    bool hasConfig, config;   // hasConfig: the key is there at all, which a folder entry may not have
    bool hasReplaces;
    std::vector<std::string> replaces;   // acceptable hashes of the stock file

    ManifestFile() : mode(0), hasConfig(false), config(false), hasReplaces(false) {}
};

struct ManifestLink {
    std::string path, target;
};

enum HookKind { kPreInstall, kPostInstall, kPreRemove, kPostRemove, kHookCount };
const char* hookName(HookKind kind);   // "preInstall", ...

struct ManifestHook {
    bool present;
    std::string script;
    unsigned timeout;   // seconds

    ManifestHook() : present(false), timeout(0) {}
};

// A Bluetooth serial service that a program of the package registers with the car's Bluetooth stack while it
// runs (install.serialServices). The stack keeps a registration until it is taken away or the stack restarts, so a
// program that ends without taking its own away leaves it behind, and phones go on opening links to a service
// that nothing answers. The declaration is what lets the agent take it away then (serial_services.hpp).
struct ManifestSerialService {
    std::string name;      // the name the registration is filed under
    std::string program;   // the file name of the program that registers it, without a folder
};

// One file the package places: an install.files entry, or one file of a folder entry.
struct Placement {
    std::string src;    // path in the archive
    std::string dest;   // destination as the manifest writes it: ${app}/..., ${data}/..., ${plugins}/... or /...
    size_t file;        // index into Manifest::files of the entry it comes from
    size_t content;     // index into Manifest::contents
};

struct Manifest {
    std::string id, name, version;
    std::string agentRange;   // requires.agent; empty when absent
    std::vector<ManifestDependency> depends, conflicts;
    std::vector<ManifestContent> contents;
    std::vector<ManifestFile> files;
    std::vector<ManifestLink> links;
    bool hasHooks;            // install.hooks is there and not empty
    ManifestHook hooks[kHookCount];
    std::string reboot;       // "none", "hmi-restart" or "required"
    std::vector<ManifestSerialService> serialServices;
    // logs[].path: the log files a controller may pull (files.list, files.fetch), as the manifest writes them.
    std::vector<std::string> logs;

    std::vector<Placement> placements;   // files, with folder entries expanded, in manifest order

    Manifest() : hasHooks(false), reboot("none") {}
};

// Checks `len` bytes of manifest.json and fills *out. On a refusal returns false with error->code set to
// bad-manifest or unsupported-manifest; *out is then unspecified.
bool parseManifest(const void* data, size_t len, Manifest* out, Error* error);

// The flags shown before an install (runs-scripts, modifies-system, needs-reboot), worked out from the manifest
// so that a source cannot understate them.
std::vector<std::string> riskFlags(const Manifest& manifest);

// True for a relative path as the archive and the manifest write one: segments separated by single slashes, none of
// them empty, "." or "..", and no backslash or control character. With `folder`, one slash at the end is allowed.
bool validRelativePath(const std::string& path, bool folder);

// The forms the manifest schema and the protocol share: a package id, a version, a name of 1 to 64 characters shown
// to the user (a package's, a source's, a phone's), and a SHA-256 as 64 lowercase hex characters.
bool validPackageId(const std::string& id);
bool validVersion(const std::string& version);
bool validDisplayName(const std::string& name);
bool validSha256Hex(const std::string& text);

// The forms of a serial service's two members, as the schema has them. A name is 1 to 32 of [A-Za-z0-9_-], which
// is what the Bluetooth stack's registration takes, and neither the agent's own service name nor the car's. A
// program is a file name of 1 to 64 of [A-Za-z0-9._+-], and not "." or "..".
bool validSerialServiceName(const std::string& name);
bool validProgramName(const std::string& program);

// The form of a logs entry's path ($defs/log/path): ${app}/, ${data}/ or ${tmp}/ and a relative path, or an
// absolute path under /tmp or /var that is not in /tmp/nbtapp or /var/opt/nbtapp, the agent's own folders. Its
// segments are not empty, "." or "..", and only the last may hold the wildcards * and ?. No slash at the end, at
// most 255 characters.
bool validLogPath(const std::string& path);

// True for a destination inside one of the package's own places: ${app}/, ${data}/ or ${plugins}/.
bool inOwnDirs(const std::string& dest);

}  // namespace nbt

#endif

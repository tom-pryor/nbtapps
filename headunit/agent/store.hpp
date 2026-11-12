// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_STORE_HPP
#define NBT_AGENT_STORE_HPP

// What the agent keeps on disk, and where. The head unit is the source of truth for what is installed, so
// everything an uninstall needs is here and nothing depends on a phone or a catalogue.
//
//   <state>/installed/<id>/     one folder per installed package
//       manifest.json           as signed
//       manifest.sig
//       record.json             what the install did: the key that signed it, each file placed with its hash, the
//                               links, the folders it made
//       backup/<n>              the stock files it replaced, to put back on uninstall
//       install.log             what the agent and the hooks said during the install
//       hooks/                  the remove hooks, at their paths from the archive: hooks/scripts/pre-remove.sh
//   <state>/journal/            the step in progress, see journal.hpp; empty between steps
//   <state>/install.log         the log of every step, the failed ones included; rotated to install.log.1
//   <staging>/blobs/            archives fetched from a phone, named by their SHA-256; partial ones end in .part
//   <staging>/unpack/           the package being installed, unpacked
//
// Staging may be on a file system that keeps its files through a power cut. Nothing in it is wanted after one, so
// the agent empties it then (clearStaging), and nothing written there is synced.
//
// Destinations are stored as the manifest writes them (${app}/bin/x), and resolved when they are used. ${app} is
// the platform's folder for the package's id. For the agent's own package (agent_home.hpp) it is the folder of
// that version in the agent's own folder, so two versions of the agent never share a path.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <string>
#include <vector>

#include "manifest.hpp"
#include "platform.hpp"

namespace nbt {

struct InstalledFile {
    std::string dest;      // as in the manifest
    std::string sha256;    // of the package's file, which is what was placed unless `kept`
    uint64_t size;
    unsigned mode;
    bool config;
    bool kept;             // a config file that was there already and was left as it was
    std::string backup;    // "backup/<n>" inside the record folder if a stock file was replaced, else empty

    InstalledFile() : size(0), mode(0), config(false), kept(false) {}
};

struct InstalledLink {
    std::string path, target;
};

struct InstalledPackage {
    Manifest manifest;
    std::string key;                    // the public key that signed it, as hex
    std::vector<InstalledFile> files;
    std::vector<InstalledLink> links;
    std::vector<std::string> dirs;      // folders the install made, as destinations, parents before children
};

class Store {
public:
    explicit Store(const Platform* platform) : platform_(platform) {}

    // Makes the folders that must exist. Safe to call again.
    bool init(std::string* error);
    // Empties the blob cache and the unpack folder, and leaves both there.
    bool clearStaging(std::string* error);

    std::string installedDir() const { return platform_->stateDir() + "/installed"; }
    std::string recordDir(const std::string& id) const { return installedDir() + "/" + id; }
    std::string journalDir() const { return platform_->stateDir() + "/journal"; }
    std::string logPath() const { return platform_->stateDir() + "/install.log"; }
    std::string blobDir() const { return platform_->stagingDir() + "/blobs"; }
    std::string blobPath(const std::string& sha256) const { return blobDir() + "/" + sha256; }
    std::string unpackDir() const { return platform_->stagingDir() + "/unpack"; }

    // The ids of the installed packages, sorted.
    bool list(std::vector<std::string>* ids, std::string* error) const;
    bool isInstalled(const std::string& id) const;
    // One record, read from `dir` (a record folder). False with *error set if it is missing or damaged.
    bool loadFrom(const std::string& dir, InstalledPackage* out, std::string* error) const;
    bool load(const std::string& id, InstalledPackage* out, std::string* error) const;
    bool loadAll(std::vector<InstalledPackage>* out, std::string* error) const;

    // The text of record.json for a package.
    static std::string encodeRecord(const InstalledPackage& package);

    // ${app} of the package with this manifest.
    std::string appDir(const Manifest& of) const;
    // A destination as a manifest writes it -> the real path, for the package with this manifest.
    std::string resolve(const Manifest& of, const std::string& dest) const;

private:
    const Platform* platform_;
};

}  // namespace nbt

#endif

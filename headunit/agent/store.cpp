// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The state store: see store.hpp.

#include "store.hpp"

#include <stdio.h>

#include "agent_home.hpp"
#include "fsutil.hpp"
#include "json.hpp"
#include "json_writer.hpp"

namespace nbt {

namespace {

const int kRecordVersion = 1;
const size_t kMaxRecordBytes = 16u << 20;

std::string modeText(unsigned mode) {
    char text[8];
    snprintf(text, sizeof text, "0%03o", mode & 0777);
    return text;
}

bool parseModeText(const std::string& text, unsigned* out) {
    if (text.size() != 4 || text[0] != '0') return false;
    unsigned mode = 0;
    for (size_t i = 1; i < 4; ++i) {
        if (text[i] < '0' || text[i] > '7') return false;
        mode = mode * 8 + (unsigned)(text[i] - '0');
    }
    *out = mode;
    return true;
}

}  // namespace

bool Store::init(std::string* error) {
    return fs::makeDirs(installedDir(), error) && fs::makeDirs(journalDir(), error) && fs::makeDirs(blobDir(), error) &&
           fs::makeDirs(unpackDir(), error);
}

bool Store::clearStaging(std::string* error) {
    // Not removed and made again: at a start that finds both empty, which is nearly every one, nothing is written.
    return fs::emptyDir(blobDir(), error) && fs::emptyDir(unpackDir(), error);
}

bool Store::list(std::vector<std::string>* ids, std::string* error) const {
    std::vector<std::string> names;
    ids->clear();
    if (!fs::listDir(installedDir(), &names, error)) return false;
    for (size_t i = 0; i < names.size(); ++i) {
        if (fs::kindOf(recordDir(names[i]) + "/record.json") == fs::kFile) ids->push_back(names[i]);
    }
    return true;
}

bool Store::isInstalled(const std::string& id) const { return fs::kindOf(recordDir(id) + "/record.json") == fs::kFile; }

std::string Store::encodeRecord(const InstalledPackage& p) {
    json::Writer w;
    w.beginObject();
    w.key("record").integer(kRecordVersion);
    w.key("id").string(p.manifest.id);
    w.key("version").string(p.manifest.version);
    w.key("key").string(p.key);
    w.key("files").beginArray();
    for (size_t i = 0; i < p.files.size(); ++i) {
        const InstalledFile& f = p.files[i];
        w.beginObject();
        w.key("dest").string(f.dest);
        w.key("sha256").string(f.sha256);
        w.key("size").integer((int64_t)f.size);
        w.key("mode").string(modeText(f.mode));
        if (f.config) w.key("config").boolean(true);
        if (f.kept) w.key("kept").boolean(true);
        if (!f.backup.empty()) w.key("backup").string(f.backup);
        w.endObject();
    }
    w.endArray();
    w.key("links").beginArray();
    for (size_t i = 0; i < p.links.size(); ++i) {
        w.beginObject().key("path").string(p.links[i].path).key("target").string(p.links[i].target).endObject();
    }
    w.endArray();
    w.key("dirs").beginArray();
    for (size_t i = 0; i < p.dirs.size(); ++i) w.string(p.dirs[i]);
    w.endArray();
    w.endObject();
    return w.text() + "\n";
}

bool Store::loadFrom(const std::string& dir, InstalledPackage* out, std::string* error) const {
    std::string why;
    std::string manifestBytes, recordBytes;
    if (!fs::readFile(dir + "/manifest.json", kMaxManifestBytes, &manifestBytes, error)) return false;
    if (!fs::readFile(dir + "/record.json", kMaxRecordBytes, &recordBytes, error)) return false;

    *out = InstalledPackage();
    Error manifestError;
    if (!parseManifest(manifestBytes.data(), manifestBytes.size(), &out->manifest, &manifestError)) {
        if (error != 0) *error = dir + "/manifest.json: " + manifestError.message;
        return false;
    }
    json::Document doc;
    if (!doc.parse(recordBytes, &why)) {
        if (error != 0) *error = dir + "/record.json: " + why;
        return false;
    }
    json::Value root = doc.root();
    json::Value files = root.find("files"), links = root.find("links"), dirs = root.find("dirs");
    bool ok = root.find("record").isInteger() && root.find("record").integer() == kRecordVersion &&
              root.find("id").isString() && root.find("id").string() == out->manifest.id &&
              root.find("version").isString() && root.find("version").string() == out->manifest.version &&
              root.find("key").isString() && root.find("key").string().size() == 64 && files.isArray() &&
              links.isArray() && dirs.isArray();
    for (size_t i = 0; ok && i < files.size(); ++i) {
        json::Value f = files.at(i);
        InstalledFile file;
        ok = f.find("dest").isString() && f.find("sha256").isString() && f.find("size").isInteger() &&
             f.find("size").integer() >= 0 && f.find("mode").isString() && parseModeText(f.find("mode").string(), &file.mode);
        if (!ok) break;
        file.dest = f.find("dest").string();
        file.sha256 = f.find("sha256").string();
        file.size = (uint64_t)f.find("size").integer();
        file.config = f.find("config").isBool() && f.find("config").boolean();
        file.kept = f.find("kept").isBool() && f.find("kept").boolean();
        if (f.find("backup").isString()) file.backup = f.find("backup").string();
        // A backup is a file directly in the record's backup folder, never a path that leads elsewhere.
        if (!file.backup.empty() && (file.backup.compare(0, 7, "backup/") != 0 || !validRelativePath(file.backup, false))) ok = false;
        out->files.push_back(file);
    }
    for (size_t i = 0; ok && i < links.size(); ++i) {
        json::Value l = links.at(i);
        InstalledLink link;
        ok = l.find("path").isString() && l.find("target").isString();
        if (!ok) break;
        link.path = l.find("path").string();
        link.target = l.find("target").string();
        out->links.push_back(link);
    }
    for (size_t i = 0; ok && i < dirs.size(); ++i) {
        ok = dirs.at(i).isString();
        if (ok) out->dirs.push_back(dirs.at(i).string());
    }
    if (!ok) {
        if (error != 0) *error = dir + "/record.json is not a record this agent wrote";
        return false;
    }
    out->key = root.find("key").string();
    return true;
}

bool Store::load(const std::string& id, InstalledPackage* out, std::string* error) const {
    if (!loadFrom(recordDir(id), out, error)) return false;
    if (out->manifest.id != id) {
        if (error != 0) *error = recordDir(id) + " holds the record of another package";
        return false;
    }
    return true;
}

bool Store::loadAll(std::vector<InstalledPackage>* out, std::string* error) const {
    std::vector<std::string> ids;
    out->clear();
    if (!list(&ids, error)) return false;
    for (size_t i = 0; i < ids.size(); ++i) {
        InstalledPackage package;
        if (!load(ids[i], &package, error)) return false;
        out->push_back(package);
    }
    return true;
}

std::string Store::appDir(const Manifest& of) const {
    if (of.id == kAgentPackageId) return AgentHome(platform_->agentHome()).versionDir(of.version);
    return platform_->appDir(of.id);
}

std::string Store::resolve(const Manifest& of, const std::string& dest) const {
    if (dest.compare(0, 6, "${app}") == 0) return appDir(of) + dest.substr(6);
    if (dest.compare(0, 7, "${data}") == 0) return platform_->dataDir(of.id) + dest.substr(7);
    if (dest.compare(0, 10, "${plugins}") == 0) return platform_->pluginsDir() + dest.substr(10);
    return platform_->systemPath(dest);
}

}  // namespace nbt

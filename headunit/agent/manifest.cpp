// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// manifest.json: see manifest.hpp. Each check* function below is one definition of spec/manifest.schema.json, and
// says which.

#include "manifest.hpp"

#include <stdio.h>

#include <map>
#include <set>

#include "json.hpp"
#include "protocol_constants.hpp"
#include "semver.hpp"
#include "utf8.hpp"

namespace nbt {

const char* hookName(HookKind kind) {
    switch (kind) {
    case kPreInstall: return "preInstall";
    case kPostInstall: return "postInstall";
    case kPreRemove: return "preRemove";
    case kPostRemove: return "postRemove";
    default: return "";
    }
}

bool inOwnDirs(const std::string& dest) {
    return dest.compare(0, 7, "${app}/") == 0 || dest.compare(0, 8, "${data}/") == 0 ||
           dest.compare(0, 11, "${plugins}/") == 0;
}

namespace {

using json::Value;

bool startsWith(const std::string& text, const char* prefix, size_t* rest) {
    size_t n = 0;
    while (prefix[n] != '\0') ++n;
    if (text.compare(0, n, prefix) != 0) return false;
    *rest = n;
    return true;
}

bool endsWithSlash(const std::string& text) { return !text.empty() && text[text.size() - 1] == '/'; }

// ---- the string forms of the schema ---------------------------------------------------------------------------

// No character from U+0000 to U+001F, and no U+007F.
bool noControls(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// One path segment: not empty, not "." or "..", and no slash, backslash or control character.
bool segment(const std::string& text, size_t begin, size_t end) {
    if (begin == end) return false;
    if (text[begin] == '.' && (end - begin == 1 || (end - begin == 2 && text[begin + 1] == '.'))) return false;
    for (size_t i = begin; i < end; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '/' || c == '\\' || c < 0x20) return false;
    }
    return true;
}

// Segments separated by single slashes, from `begin` to the end of the text.
bool segments(const std::string& text, size_t begin, bool trailingSlash) {
    size_t end = text.size();
    if (trailingSlash && end > begin && text[end - 1] == '/') --end;
    if (begin >= end) return false;
    size_t start = begin;
    for (size_t i = begin; i <= end; ++i) {
        if (i == end || text[i] == '/') {
            if (!segment(text, start, i)) return false;
            start = i + 1;
        }
    }
    return true;
}

// $defs/packageId: two or more labels joined by dots; a label is runs of [a-z0-9] joined by single hyphens.
bool isPackageId(const std::string& text) {
    if (utf8Length(text) > 128) return false;
    int labels = 0;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != '.') {
            char c = text[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
            if (c == '-' && (i == start || i + 1 == text.size() || text[i + 1] == '.' || text[i + 1] == '-')) return false;
            continue;
        }
        if (i == start) return false;   // an empty label
        ++labels;
        start = i + 1;
    }
    return labels >= 2;
}

// $defs/name.
bool isName(const std::string& text) {
    size_t n = utf8Length(text);
    return n >= 1 && n <= 64 && noControls(text);
}

// $defs/version and $defs/range.
bool isVersion(const std::string& text) {
    Version v;
    return text.size() <= 64 && parseVersion(text, &v);
}
bool isRange(const std::string& text) {
    Range r;
    return text.size() <= 256 && parseRange(text, &r);
}

// $defs/sha256.
bool isSha256(const std::string& text) {
    if (text.size() != 64) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// $defs/path: a relative path inside the archive. $defs/file/src: the same, or a folder with a slash at the end.
bool isPath(const std::string& text) { return utf8Length(text) <= 255 && segments(text, 0, false); }
bool isSrc(const std::string& text) { return utf8Length(text) <= 255 && segments(text, 0, true); }

// $defs/file/dest: ${app}/, ${data}/ or ${plugins}/ and then nothing or a relative path; or an absolute path.
// Either may end in a slash, for a folder.
bool isDest(const std::string& text) {
    if (utf8Length(text) > 255) return false;
    size_t rest = 0;
    if (startsWith(text, "${app}/", &rest) || startsWith(text, "${data}/", &rest) ||
        startsWith(text, "${plugins}/", &rest)) {
        return rest == text.size() || segments(text, rest, true);
    }
    return !text.empty() && text[0] == '/' && segments(text, 1, true);
}

// $defs/link/path: an absolute path, or one under ${app}, ${data} or ${plugins}; no slash at the end.
bool isLinkPath(const std::string& text) {
    if (utf8Length(text) > 255) return false;
    size_t rest = 0;
    if (!startsWith(text, "${app}", &rest) && !startsWith(text, "${data}", &rest) &&
        !startsWith(text, "${plugins}", &rest)) {
        rest = 0;
    }
    return rest < text.size() && text[rest] == '/' && segments(text, rest + 1, false);
}

// $defs/link/target.
bool isLinkTarget(const std::string& text) {
    size_t n = utf8Length(text);
    return n >= 1 && n <= 255 && noControls(text);
}

// $defs/file/mode: 0 and three octal digits.
bool parseMode(const std::string& text, unsigned* out) {
    if (text.size() != 4 || text[0] != '0') return false;
    unsigned mode = 0;
    for (size_t i = 1; i < 4; ++i) {
        if (text[i] < '0' || text[i] > '7') return false;
        mode = mode * 8 + (unsigned)(text[i] - '0');
    }
    *out = mode;
    return true;
}

// $defs/serialService/name: 1 to 32 of [A-Za-z0-9_-], and not a name that is taken: the agent's own service
// (protocol_constants.hpp) or the car's serial service, "SPP" (serial_link.hpp).
bool isSerialServiceName(const std::string& text) {
    if (text.empty() || text.size() > 32) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return text != kSerialServiceName && text != "SPP";
}

// $defs/serialService/program: a file name of 1 to 64 of [A-Za-z0-9._+-], and not "." or "..".
bool isProgramName(const std::string& text) {
    if (text.empty() || text.size() > 64 || text == "." || text == "..") return false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '+' || c == '-')) {
            return false;
        }
    }
    return true;
}

// $defs/log/path: see validLogPath in manifest.hpp.
bool isLogPath(const std::string& text) {
    if (utf8Length(text) > 255) return false;
    size_t rest = 0;
    if (startsWith(text, "${app}", &rest) || startsWith(text, "${data}", &rest) || startsWith(text, "${tmp}", &rest)) {
        // the package's own folders
    } else if (startsWith(text, "/tmp", &rest)) {
        if (text.compare(0, 11, "/tmp/nbtapp") == 0 && (text.size() == 11 || text[11] == '/')) return false;
    } else if (startsWith(text, "/var", &rest)) {
        if (text.compare(0, 15, "/var/opt/nbtapp") == 0 && (text.size() == 15 || text[15] == '/')) return false;
    } else {
        return false;
    }
    if (rest >= text.size() || text[rest] != '/' || !segments(text, rest + 1, false)) return false;
    size_t last = text.rfind('/');
    for (size_t i = rest; i < last; ++i) {
        if (text[i] == '*' || text[i] == '?') return false;   // a wildcard outside the last segment
    }
    return true;
}

// ---- the schema, walked in the order of the text --------------------------------------------------------------

class Checker {
public:
    Checker(Manifest* out, Error* error) : m_(out), error_(error) {}

    bool run(const Value& root) {
        static const char* const required[] = {"manifestVersion", "id", "name", "version", "contents", 0};
        if (!has(root, required, "manifest")) return false;
        for (size_t i = 0; i < root.size(); ++i) {
            const std::string& key = root.key(i);
            Value v = root.at(i);
            if (key == "id") {
                if (!text(v, isPackageId, "manifest.id", &m_->id)) return false;
            } else if (key == "name") {
                if (!text(v, isName, "manifest.name", &m_->name)) return false;
            } else if (key == "version") {
                if (!text(v, isVersion, "manifest.version", &m_->version)) return false;
            } else if (key == "requires") {
                if (!requiresObject(v)) return false;
            } else if (key == "depends") {
                if (!dependencies(v, "manifest.depends", &m_->depends)) return false;
            } else if (key == "conflicts") {
                if (!dependencies(v, "manifest.conflicts", &m_->conflicts)) return false;
            } else if (key == "contents") {
                if (!contents(v)) return false;
            } else if (key == "install") {
                if (!install(v)) return false;
            } else if (key == "logs") {
                if (!logs(v)) return false;
            }
            // manifestVersion was checked by the caller; any other key at this level is ignored.
        }
        return crossFields();
    }

private:
    bool bad(const std::string& where, const char* what) { return error_->fail("bad-manifest", where + " " + what); }
    bool unknown(const std::string& where, const std::string& key) {
        return error_->fail("unsupported-manifest", where + " has the unknown key \"" + key + "\"");
    }

    bool has(const Value& object, const char* const* required, const std::string& where) {
        for (; *required != 0; ++required) {
            if (!object.find(*required).present()) return bad(where, (std::string("has no \"") + *required + "\"").c_str());
        }
        return true;
    }

    bool text(const Value& v, bool (*form)(const std::string&), const std::string& where, std::string* out) {
        if (!v.isString()) return bad(where, "must be a string");
        if (!form(v.string())) return bad(where, "has a form or a length that is not allowed");
        *out = v.string();
        return true;
    }

    bool integer(const Value& v, int64_t low, int64_t high, const std::string& where, int64_t* out) {
        if (!v.isInteger()) return bad(where, "must be an integer");
        if (v.integer() < low || v.integer() > high) return bad(where, "is out of range");
        *out = v.integer();
        return true;
    }

    static std::string item(const std::string& where, size_t i) {
        char index[32];
        snprintf(index, sizeof index, "[%lu]", (unsigned long)i);
        return where + index;
    }

    // properties/requires: closed.
    bool requiresObject(const Value& v) {
        if (!v.isObject()) return bad("manifest.requires", "must be an object");
        for (size_t i = 0; i < v.size(); ++i) {
            if (v.key(i) != "agent") return unknown("manifest.requires", v.key(i));
            if (!text(v.at(i), isRange, "manifest.requires.agent", &m_->agentRange)) return false;
        }
        return true;
    }

    // properties/depends and conflicts: arrays of $defs/dependency, which is open.
    bool dependencies(const Value& v, const std::string& where, std::vector<ManifestDependency>* out) {
        static const char* const required[] = {"id", 0};
        if (!v.isArray()) return bad(where, "must be an array");
        for (size_t i = 0; i < v.size(); ++i) {
            Value d = v.at(i);
            std::string at = item(where, i);
            if (!d.isObject()) return bad(at, "must be an object");
            if (!has(d, required, at)) return false;
            ManifestDependency dep;
            for (size_t k = 0; k < d.size(); ++k) {
                if (d.key(k) == "id") {
                    if (!text(d.at(k), isPackageId, at + ".id", &dep.id)) return false;
                } else if (d.key(k) == "version") {
                    if (!text(d.at(k), isRange, at + ".version", &dep.range)) return false;
                }
            }
            out->push_back(dep);
        }
        return true;
    }

    // properties/contents: an array of $defs/content, which is open.
    bool contents(const Value& v) {
        static const char* const required[] = {"path", "size", "sha256", 0};
        if (!v.isArray()) return bad("manifest.contents", "must be an array");
        for (size_t i = 0; i < v.size(); ++i) {
            Value c = v.at(i);
            std::string at = item("manifest.contents", i);
            if (!c.isObject()) return bad(at, "must be an object");
            if (!has(c, required, at)) return false;
            ManifestContent content;
            content.size = 0;
            for (size_t k = 0; k < c.size(); ++k) {
                if (c.key(k) == "path") {
                    if (!text(c.at(k), isPath, at + ".path", &content.path)) return false;
                } else if (c.key(k) == "size") {
                    int64_t size = 0;
                    if (!integer(c.at(k), 0, 4294967294LL, at + ".size", &size)) return false;
                    content.size = (uint64_t)size;
                } else if (c.key(k) == "sha256") {
                    if (!text(c.at(k), isSha256, at + ".sha256", &content.sha256)) return false;
                }
            }
            m_->contents.push_back(content);
        }
        return true;
    }

    // properties/install: closed.
    bool install(const Value& v) {
        if (!v.isObject()) return bad("manifest.install", "must be an object");
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string& key = v.key(i);
            if (key == "files") {
                if (!files(v.at(i))) return false;
            } else if (key == "links") {
                if (!links(v.at(i))) return false;
            } else if (key == "hooks") {
                if (!hooks(v.at(i))) return false;
            } else if (key == "reboot") {
                Value r = v.at(i);
                if (!r.isString() || (r.string() != "none" && r.string() != "hmi-restart" && r.string() != "required")) {
                    return bad("manifest.install.reboot", "must be one of: none, hmi-restart, required");
                }
                m_->reboot = r.string();
            } else if (key == "serialServices") {
                if (!serialServices(v.at(i))) return false;
            } else {
                return unknown("manifest.install", key);
            }
        }
        return true;
    }

    // $defs/serialService: closed.
    bool serialServices(const Value& v) {
        static const char* const required[] = {"name", "program", 0};
        if (!v.isArray()) return bad("manifest.install.serialServices", "must be an array");
        for (size_t i = 0; i < v.size(); ++i) {
            Value s = v.at(i);
            std::string at = item("manifest.install.serialServices", i);
            if (!s.isObject()) return bad(at, "must be an object");
            if (!has(s, required, at)) return false;
            ManifestSerialService service;
            for (size_t k = 0; k < s.size(); ++k) {
                if (s.key(k) == "name") {
                    if (!text(s.at(k), isSerialServiceName, at + ".name", &service.name)) return false;
                } else if (s.key(k) == "program") {
                    if (!text(s.at(k), isProgramName, at + ".program", &service.program)) return false;
                } else {
                    return unknown(at, s.key(k));
                }
            }
            m_->serialServices.push_back(service);
        }
        return true;
    }

    // properties/logs: at most 16 of $defs/log, which is open.
    bool logs(const Value& v) {
        static const char* const required[] = {"path", 0};
        if (!v.isArray()) return bad("manifest.logs", "must be an array");
        if (v.size() > kMaxLogs) return bad("manifest.logs", "has more than 16 entries");
        for (size_t i = 0; i < v.size(); ++i) {
            Value l = v.at(i);
            std::string at = item("manifest.logs", i);
            if (!l.isObject()) return bad(at, "must be an object");
            if (!has(l, required, at)) return false;
            std::string path;
            for (size_t k = 0; k < l.size(); ++k) {
                if (l.key(k) == "path" && !text(l.at(k), isLogPath, at + ".path", &path)) return false;
            }
            m_->logs.push_back(path);
        }
        return true;
    }

    // $defs/file: closed.
    bool files(const Value& v) {
        static const char* const required[] = {"src", "dest", "mode", 0};
        if (!v.isArray()) return bad("manifest.install.files", "must be an array");
        for (size_t i = 0; i < v.size(); ++i) {
            Value f = v.at(i);
            std::string at = item("manifest.install.files", i);
            if (!f.isObject()) return bad(at, "must be an object");
            if (!has(f, required, at)) return false;
            ManifestFile file;
            for (size_t k = 0; k < f.size(); ++k) {
                const std::string& key = f.key(k);
                Value value = f.at(k);
                if (key == "src") {
                    if (!text(value, isSrc, at + ".src", &file.src)) return false;
                } else if (key == "dest") {
                    if (!text(value, isDest, at + ".dest", &file.dest)) return false;
                } else if (key == "mode") {
                    if (!value.isString()) return bad(at + ".mode", "must be a string");
                    if (!parseMode(value.string(), &file.mode)) return bad(at + ".mode", "must be 0 and three octal digits");
                } else if (key == "config") {
                    if (!value.isBool()) return bad(at + ".config", "must be true or false");
                    file.hasConfig = true;
                    file.config = value.boolean();
                } else if (key == "replaces") {
                    if (!replaces(value, at + ".replaces", &file)) return false;
                } else {
                    return unknown(at, key);
                }
            }
            m_->files.push_back(file);
        }
        return true;
    }

    // $defs/file/replaces: closed, with one or more hashes.
    bool replaces(const Value& v, const std::string& where, ManifestFile* file) {
        static const char* const required[] = {"sha256", 0};
        if (!v.isObject()) return bad(where, "must be an object");
        if (!has(v, required, where)) return false;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v.key(i) != "sha256") return unknown(where, v.key(i));
            Value list = v.at(i);
            if (!list.isArray()) return bad(where + ".sha256", "must be an array");
            if (list.size() < 1) return bad(where + ".sha256", "needs at least one hash");
            for (size_t k = 0; k < list.size(); ++k) {
                std::string hash;
                if (!text(list.at(k), isSha256, item(where + ".sha256", k), &hash)) return false;
                file->replaces.push_back(hash);
            }
        }
        file->hasReplaces = true;
        return true;
    }

    // $defs/link: closed.
    bool links(const Value& v) {
        static const char* const required[] = {"path", "target", 0};
        if (!v.isArray()) return bad("manifest.install.links", "must be an array");
        for (size_t i = 0; i < v.size(); ++i) {
            Value l = v.at(i);
            std::string at = item("manifest.install.links", i);
            if (!l.isObject()) return bad(at, "must be an object");
            if (!has(l, required, at)) return false;
            ManifestLink link;
            for (size_t k = 0; k < l.size(); ++k) {
                if (l.key(k) == "path") {
                    if (!text(l.at(k), isLinkPath, at + ".path", &link.path)) return false;
                } else if (l.key(k) == "target") {
                    if (!text(l.at(k), isLinkTarget, at + ".target", &link.target)) return false;
                } else {
                    return unknown(at, l.key(k));
                }
            }
            m_->links.push_back(link);
        }
        return true;
    }

    // install/hooks: closed, each member a $defs/hook, which is closed too.
    bool hooks(const Value& v) {
        static const char* const required[] = {"script", "timeout", 0};
        if (!v.isObject()) return bad("manifest.install.hooks", "must be an object");
        for (size_t i = 0; i < v.size(); ++i) {
            int kind = -1;
            for (int h = 0; h < kHookCount; ++h) {
                if (v.key(i) == hookName((HookKind)h)) kind = h;
            }
            if (kind < 0) return unknown("manifest.install.hooks", v.key(i));
            Value h = v.at(i);
            std::string at = "manifest.install.hooks." + v.key(i);
            if (!h.isObject()) return bad(at, "must be an object");
            if (!has(h, required, at)) return false;
            ManifestHook hook;
            for (size_t k = 0; k < h.size(); ++k) {
                if (h.key(k) == "script") {
                    if (!text(h.at(k), isPath, at + ".script", &hook.script)) return false;
                } else if (h.key(k) == "timeout") {
                    int64_t timeout = 0;
                    if (!integer(h.at(k), 1, 600, at + ".timeout", &timeout)) return false;
                    hook.timeout = (unsigned)timeout;
                } else {
                    return unknown(at, h.key(k));
                }
            }
            hook.present = true;
            m_->hooks[kind] = hook;
            m_->hasHooks = true;
        }
        return true;
    }

    // ---- the rules that span fields ---------------------------------------------------------------------------

    bool place(const std::string& dest) {
        if (dest.size() > kMaxPathBytes) return bad("manifest", ("places a path longer than 255 bytes: " + dest).c_str());
        if (!placed_.insert(dest).second) return bad("manifest", ("places " + dest + " twice").c_str());
        return true;
    }

    bool crossFields() {
        std::map<std::string, size_t> paths;   // sorted by bytes, which for UTF-8 is the order of the characters
        for (size_t i = 0; i < m_->contents.size(); ++i) {
            const std::string& path = m_->contents[i].path;
            if (path == "manifest.json" || path == "manifest.sig") return bad("manifest", ("lists " + path + " in contents").c_str());
            if (path.size() > kMaxPathBytes) return bad("manifest", "lists a path longer than 255 bytes");
            if (!paths.insert(std::make_pair(path, i)).second) return bad("manifest", ("lists " + path + " twice").c_str());
        }

        for (size_t i = 0; i < m_->files.size(); ++i) {
            const ManifestFile& file = m_->files[i];
            if (endsWithSlash(file.src) != endsWithSlash(file.dest)) {
                return bad("manifest", ("maps " + file.src + " to " + file.dest +
                                        ": a folder goes to a folder and a file to a file").c_str());
            }
            Placement p;
            p.file = i;
            if (endsWithSlash(file.src)) {
                std::map<std::string, size_t>::const_iterator it = paths.lower_bound(file.src);
                if (it == paths.end() || it->first.compare(0, file.src.size(), file.src) != 0) {
                    return bad("manifest", ("installs the folder " + file.src + ", which holds nothing listed in contents").c_str());
                }
                if (file.hasConfig || file.hasReplaces) {
                    return bad("manifest", ("sets config or replaces on the folder " + file.src).c_str());
                }
                for (; it != paths.end() && it->first.compare(0, file.src.size(), file.src) == 0; ++it) {
                    p.src = it->first;
                    p.dest = file.dest + it->first.substr(file.src.size());
                    p.content = it->second;
                    if (!place(p.dest)) return false;
                    m_->placements.push_back(p);
                }
            } else {
                std::map<std::string, size_t>::const_iterator it = paths.find(file.src);
                if (it == paths.end()) return bad("manifest", ("installs " + file.src + ", which is not listed in contents").c_str());
                p.src = file.src;
                p.dest = file.dest;
                p.content = it->second;
                if (!place(p.dest)) return false;
                m_->placements.push_back(p);
            }
        }
        for (size_t i = 0; i < m_->links.size(); ++i) {
            if (!place(m_->links[i].path)) return false;
        }
        for (int h = 0; h < kHookCount; ++h) {
            if (m_->hooks[h].present && paths.find(m_->hooks[h].script) == paths.end()) {
                return bad("manifest", ("names " + m_->hooks[h].script + " as a hook, which is not listed in contents").c_str());
            }
        }
        std::set<std::string> services;
        for (size_t i = 0; i < m_->serialServices.size(); ++i) {
            const std::string& name = m_->serialServices[i].name;
            if (!services.insert(name).second) return bad("manifest", ("declares the serial service " + name + " twice").c_str());
        }
        std::set<std::string> logs;
        for (size_t i = 0; i < m_->logs.size(); ++i) {
            if (!logs.insert(m_->logs[i]).second) return bad("manifest", ("names the log " + m_->logs[i] + " twice").c_str());
        }
        return true;
    }

    Manifest* m_;
    Error* error_;
    std::set<std::string> placed_;
};

}  // namespace

bool validRelativePath(const std::string& path, bool folder) { return segments(path, 0, folder); }
bool validPackageId(const std::string& id) { return isPackageId(id); }
bool validDisplayName(const std::string& name) { return isName(name); }
bool validSha256Hex(const std::string& text) { return isSha256(text); }
bool validVersion(const std::string& version) { return isVersion(version); }
bool validSerialServiceName(const std::string& name) { return isSerialServiceName(name); }
bool validProgramName(const std::string& program) { return isProgramName(program); }
bool validLogPath(const std::string& path) { return isLogPath(path); }

bool parseManifest(const void* data, size_t len, Manifest* out, Error* error) {
    if (len > kMaxManifestBytes) return error->fail("bad-manifest", "manifest is larger than 1 MiB");
    json::Document doc;
    std::string why;
    if (!doc.parse(data, len, &why)) return error->fail("bad-manifest", "manifest is not inside the JSON profile: " + why);

    Value version = doc.root().find("manifestVersion");
    if (!version.isInteger()) return error->fail("bad-manifest", "manifest has no integer manifestVersion");
    if (version.integer() != kManifestVersion) {
        return error->fail("unsupported-manifest", "this manifestVersion is not supported");
    }

    *out = Manifest();
    Checker checker(out, error);
    return checker.run(doc.root());
}

std::vector<std::string> riskFlags(const Manifest& manifest) {
    std::vector<std::string> flags;
    if (manifest.hasHooks) flags.push_back("runs-scripts");
    bool system = false;
    for (size_t i = 0; i < manifest.files.size(); ++i) {
        if (!inOwnDirs(manifest.files[i].dest) || manifest.files[i].hasReplaces) system = true;
    }
    for (size_t i = 0; i < manifest.links.size(); ++i) {
        if (!inOwnDirs(manifest.links[i].path)) system = true;
    }
    if (system) flags.push_back("modifies-system");
    if (manifest.reboot != "none") flags.push_back("needs-reboot");
    return flags;
}

}  // namespace nbt

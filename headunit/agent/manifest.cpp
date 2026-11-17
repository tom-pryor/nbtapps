// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// manifest.json: see manifest.hpp. Each check* function below is one definition of spec/manifest.schema.json, and
// says which.

#include "manifest.hpp"

#include <stdio.h>
#include <string.h>

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

const char* const kDefaultSettingsFile = "${data}/settings";

const char* settingTypeName(SettingType type) {
    switch (type) {
    case kSettingBool: return "bool";
    case kSettingInt: return "int";
    case kSettingText: return "text";
    case kSettingSelect: return "select";
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

// The agent's own folders, which no logs entry may name: its state, with the pairing keys, and its RAM folder.
const char* const kAgentFolders[] = {"/tmp/nbtapp", "/var/opt/nbtapp", 0};

// `text` is `folder` or is inside it.
bool inFolder(const std::string& text, const char* folder) {
    size_t n = strlen(folder);
    return text.compare(0, n, folder) == 0 && (text.size() == n || text[n] == '/');
}

// $defs/log/path: see validLogPath in manifest.hpp.
bool isLogPath(const std::string& text) {
    if (utf8Length(text) > 255) return false;
    size_t rest = 0;
    if (startsWith(text, "${app}", &rest) || startsWith(text, "${data}", &rest) || startsWith(text, "${tmp}", &rest)) {
        // the package's own folders
    } else if (const LogRoot* root = logRootOf(text)) {
        rest = strlen(root->path);
        for (size_t i = 0; kAgentFolders[i] != 0; ++i) {
            if (inFolder(text, kAgentFolders[i])) return false;
        }
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

// $defs/settingsFile: ${data}/ and a relative path, no slash at the end.
bool isSettingsFile(const std::string& text) {
    size_t rest = 0;
    return utf8Length(text) <= 255 && startsWith(text, "${data}/", &rest) && segments(text, rest, false);
}

// $defs/settingKey: [A-Z][A-Z0-9_]{0,63}.
bool isSettingKey(const std::string& text) {
    if (text.empty() || text.size() > 64 || text[0] < 'A' || text[0] > 'Z') return false;
    for (size_t i = 1; i < text.size(); ++i) {
        char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// $defs/settingDescription: at most 500 characters, no control character.
bool isSettingDescription(const std::string& text) { return utf8Length(text) <= 500 && noControls(text); }

// $defs/settingInt/unit: 1 to 16 characters, no control character.
bool isSettingUnit(const std::string& text) {
    size_t n = utf8Length(text);
    return n >= 1 && n <= 16 && noControls(text);
}

// $defs/settingChoice/value: 1 to 64 of [A-Za-z0-9._-].
bool isChoiceValue(const std::string& text) {
    if (text.empty() || text.size() > 64) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

// A string that names one of `names`, a list ending in 0.
bool oneOf(const Value& v, const char* const* names) {
    if (!v.isString()) return false;
    for (; *names != 0; ++names) {
        if (v.string() == *names) return true;
    }
    return false;
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
            } else if (key == "settings") {
                if (!settings(v)) return false;
            }
            // manifestVersion was checked by the caller; any other key at this level is ignored.
        }
        return crossFields();
    }

private:
    // An option of settings.options as the walk found it: what every option has, and the object for the rest.
    struct PendingSetting {
        std::string at, key, label, type;
        Value value;
    };

    bool bad(const std::string& where, const char* what) { return error_->fail("bad-manifest", where + " " + what); }
    bool badSetting(const std::string& key, const char* what) {
        return error_->fail("bad-manifest", "manifest gives the setting " + key + " " + what);
    }
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

    // properties/settings: open, with file and 1 to 64 options of $defs/setting, which is open. Here only what every
    // option has, whatever its type; the rest waits for settingsRules(), as nbtpkg.py's check_settings does it after
    // the schema, so that a key refused in install further down the text still decides the code.
    bool settings(const Value& v) {
        static const char* const required[] = {"options", 0};
        static const char* const option[] = {"key", "label", "type", 0};
        if (!v.isObject()) return bad("manifest.settings", "must be an object");
        if (!has(v, required, "manifest.settings")) return false;
        m_->hasSettings = true;
        m_->settingsFile = kDefaultSettingsFile;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v.key(i) == "file") {
                if (!text(v.at(i), isSettingsFile, "manifest.settings.file", &m_->settingsFile)) return false;
            } else if (v.key(i) == "options") {
                Value list = v.at(i);
                if (!list.isArray()) return bad("manifest.settings.options", "must be an array");
                if (list.size() < 1) return bad("manifest.settings.options", "needs at least one option");
                if (list.size() > kMaxSettings) return bad("manifest.settings.options", "has more than 64 options");
                for (size_t k = 0; k < list.size(); ++k) {
                    PendingSetting p;
                    p.at = item("manifest.settings.options", k);
                    p.value = list.at(k);
                    if (!p.value.isObject()) return bad(p.at, "must be an object");
                    if (!has(p.value, option, p.at)) return false;
                    for (size_t m = 0; m < p.value.size(); ++m) {
                        const std::string& key = p.value.key(m);
                        if (key == "key") {
                            if (!text(p.value.at(m), isSettingKey, p.at + ".key", &p.key)) return false;
                        } else if (key == "label") {
                            if (!text(p.value.at(m), isName, p.at + ".label", &p.label)) return false;
                        } else if (key == "type") {
                            if (!p.value.at(m).isString()) return bad(p.at + ".type", "must be a string");
                            p.type = p.value.at(m).string();
                        }
                    }
                    pending_.push_back(p);
                }
            }
        }
        return true;
    }

    // $defs/settingBool, settingInt, settingText and settingSelect, each open, and the rules of $defs/setting that
    // span fields. An option of an unknown type has only its key and label looked at.
    bool settingsRules() {
        std::set<std::string> keys;
        for (size_t i = 0; i < pending_.size(); ++i) {
            const PendingSetting& p = pending_[i];
            if (!keys.insert(p.key).second) return bad("manifest", ("names the setting " + p.key + " twice").c_str());
            size_t bytes = p.key.size() + p.label.size();
            ManifestSetting s;
            bool known = true;
            if (p.type == "bool") s.type = kSettingBool;
            else if (p.type == "int") s.type = kSettingInt;
            else if (p.type == "text") s.type = kSettingText;
            else if (p.type == "select") s.type = kSettingSelect;
            else known = false;
            if (known) {
                s.key = p.key;
                s.label = p.label;
                if (!setting(p, &s)) return false;
                bytes += s.description.size() + s.unset.size() + s.unit.size();
                if (s.type == kSettingText || s.type == kSettingSelect) bytes += s.defaultText.size();
                for (size_t k = 0; k < s.values.size(); ++k) bytes += s.values[k].value.size() + s.values[k].label.size();
            }
            if (bytes > kMaxSettingBytes) {
                return bad("manifest", ("gives the setting " + p.key + " more than 1024 bytes of text").c_str());
            }
            if (known) m_->settings.push_back(s);
        }
        return true;
    }

    // One option of a known type: its type's definition, walked in the order of the text, then the rules that span
    // its fields.
    bool setting(const PendingSetting& p, ManifestSetting* s) {
        static const char* const intRequired[] = {"min", "max", 0};
        static const char* const textRequired[] = {"maxLength", 0};
        static const char* const selectRequired[] = {"values", 0};
        static const char* const applies[] = {"now", "app-start", "hmi-restart", "reboot", 0};
        static const char* const characters[] = {"safe", "any", 0};
        const Value& o = p.value;
        const std::string& at = p.at;
        if (s->type == kSettingInt && !has(o, intRequired, at)) return false;
        if (s->type == kSettingText && !has(o, textRequired, at)) return false;
        if (s->type == kSettingSelect && !has(o, selectRequired, at)) return false;
        bool hasUnset = false;
        for (size_t i = 0; i < o.size(); ++i) {
            const std::string& key = o.key(i);
            Value v = o.at(i);
            std::string where = at + "." + key;
            if (key == "description") {
                if (!text(v, isSettingDescription, where, &s->description)) return false;
            } else if (key == "unset") {
                if (!text(v, isName, where, &s->unset)) return false;
                hasUnset = true;
            } else if (key == "applies") {
                if (!oneOf(v, applies)) return bad(where, "must be one of: now, app-start, hmi-restart, reboot");
                s->applies = v.string();
            } else if (key == "advanced") {
                if (!v.isBool()) return bad(where, "must be true or false");
                s->advanced = v.boolean();
            } else if (key == "default") {
                s->hasDefault = true;
                if (s->type == kSettingBool) {
                    if (!v.isBool()) return bad(where, "must be true or false");
                    s->defaultBool = v.boolean();
                } else if (s->type == kSettingInt) {
                    if (!integer(v, -2147483648LL, 2147483647LL, where, &s->defaultInt)) return false;
                } else {
                    if (!v.isString()) return bad(where, "must be a string");
                    s->defaultText = v.string();
                }
            } else if (s->type == kSettingInt && (key == "min" || key == "max")) {
                if (!integer(v, -2147483648LL, 2147483647LL, where, key == "min" ? &s->min : &s->max)) return false;
            } else if (s->type == kSettingInt && key == "unit") {
                if (!text(v, isSettingUnit, where, &s->unit)) return false;
            } else if (s->type == kSettingText && key == "maxLength") {
                int64_t n = 0;
                if (!integer(v, 1, (int64_t)kSettingsMaxValueBytes, where, &n)) return false;
                s->maxLength = (size_t)n;
            } else if (s->type == kSettingText && key == "characters") {
                if (!oneOf(v, characters)) return bad(where, "must be one of: safe, any");
                s->anyCharacters = v.string() == "any";
            } else if (s->type == kSettingSelect && key == "values") {
                if (!choices(v, where, &s->values)) return false;
            }
        }

        if (s->hasDefault && hasUnset) {
            return badSetting(s->key, "both a default and an unset text");
        }
        if (s->type == kSettingInt) {
            if (s->min > s->max) return badSetting(s->key, "a min above its max");
            if (s->hasDefault && (s->defaultInt < s->min || s->defaultInt > s->max)) {
                return badSetting(s->key, "a default outside its limits");
            }
        } else if (s->type == kSettingText) {
            if (s->hasDefault && !validSettingText(s->defaultText, s->maxLength, s->anyCharacters)) {
                return badSetting(s->key, "a default it does not take");
            }
        } else if (s->type == kSettingSelect) {
            std::set<std::string> seen;
            bool found = false;
            for (size_t k = 0; k < s->values.size(); ++k) {
                if (!seen.insert(s->values[k].value).second) {
                    return badSetting(s->key, "a value twice");
                }
                if (s->values[k].value == s->defaultText) found = true;
            }
            if (s->hasDefault && !found) {
                return badSetting(s->key, "a default that is not one of its values");
            }
        }
        return true;
    }

    // $defs/settingSelect/values: 2 to 16 of $defs/settingChoice, which is open.
    bool choices(const Value& v, const std::string& where, std::vector<ManifestSettingChoice>* out) {
        static const char* const required[] = {"value", "label", 0};
        if (!v.isArray()) return bad(where, "must be an array");
        if (v.size() < 2) return bad(where, "needs at least two values");
        if (v.size() > kMaxSettingValues) return bad(where, "has more than 16 values");
        for (size_t i = 0; i < v.size(); ++i) {
            Value c = v.at(i);
            std::string at = item(where, i);
            if (!c.isObject()) return bad(at, "must be an object");
            if (!has(c, required, at)) return false;
            ManifestSettingChoice choice;
            for (size_t k = 0; k < c.size(); ++k) {
                if (c.key(k) == "value") {
                    if (!text(c.at(k), isChoiceValue, at + ".value", &choice.value)) return false;
                } else if (c.key(k) == "label") {
                    if (!text(c.at(k), isName, at + ".label", &choice.label)) return false;
                }
            }
            out->push_back(choice);
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
        return settingsRules();
    }

    Manifest* m_;
    Error* error_;
    std::set<std::string> placed_;
    std::vector<PendingSetting> pending_;
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

namespace {
const LogRoot kLogRoots[] = {{"/tmp"}, {"/var"}};
}  // namespace

const LogRoot* logRootOf(const std::string& path) {
    for (size_t i = 0; i < sizeof kLogRoots / sizeof kLogRoots[0]; ++i) {
        size_t n = strlen(kLogRoots[i].path);
        if (path.compare(0, n, kLogRoots[i].path) == 0 && path.size() > n && path[n] == '/') return &kLogRoots[i];
    }
    return 0;
}
bool validSettingsFile(const std::string& path) { return isSettingsFile(path); }

bool validSettingText(const std::string& value, size_t maxLength, bool anyCharacters) {
    if (value.empty() || value.size() > maxLength) return false;
    if (anyCharacters) return noControls(value) && value[0] != ' ' && value[value.size() - 1] != ' ';
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == ':' || c == '/' || c == '@' || c == '+' || c == ',' || c == '-')) {
            return false;
        }
    }
    return true;
}

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

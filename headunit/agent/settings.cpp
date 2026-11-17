// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The settings of an installed package: see settings.hpp.

#include "settings.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#include "fsutil.hpp"
#include "json_writer.hpp"
#include "log_files.hpp"
#include "settings_file.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

// ASCII lowercase, for the words a bool reads.
std::string lower(const std::string& text) {
    std::string out = text;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    }
    return out;
}

std::string decimal(int64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", (long long)value);
    return text;
}

}  // namespace

int settingBoolWord(const std::string& text) {
    std::string word = lower(text);
    if (word == "1" || word == "on" || word == "yes" || word == "true") return 1;
    if (word == "0" || word == "off" || word == "no" || word == "false") return 0;
    return -1;
}

bool settingIntText(const std::string& text, int64_t* out) {
    size_t at = !text.empty() && text[0] == '-' ? 1 : 0;
    if (at == text.size()) return false;
    const int64_t kCap = 1LL << 40;
    int64_t value = 0;
    for (size_t i = at; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        if (value < kCap) value = value * 10 + (text[i] - '0');
    }
    *out = at == 1 ? -value : value;
    return true;
}

const char* settingStateName(SettingState state) {
    switch (state) {
    case kSettingSet: return "set";
    case kSettingByHand: return "by-hand";
    default: return "unset";
    }
}

std::string settingRaw(const std::string& text) {
    size_t n = text.size();
    if (n > kSettingsMaxRawBytes) {
        n = kSettingsMaxRawBytes;
        // Back to the start of a character that the cut would split: a lead byte is at most three bytes before.
        for (int back = 0; back < 3 && n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80; ++back) --n;
    }
    return sanitizeUtf8(text.substr(0, n), 0);
}

SettingState readSettingValue(const ManifestSetting& option, const std::string& text, std::string* json, std::string* raw) {
    json::Writer w;
    bool taken = false;
    switch (option.type) {
    case kSettingBool: {
        int word = settingBoolWord(text);
        if (word >= 0) {
            w.boolean(word == 1);
            taken = true;
        }
        break;
    }
    case kSettingInt: {
        int64_t value = 0;
        if (settingIntText(text, &value) && value >= option.min && value <= option.max) {
            w.integer(value);
            taken = true;
        }
        break;
    }
    case kSettingText:
        if (validUtf8(text) && validSettingText(text, option.maxLength, option.anyCharacters)) {
            w.string(text);
            taken = true;
        }
        break;
    case kSettingSelect:
        for (size_t i = 0; i < option.values.size() && !taken; ++i) {
            if (option.values[i].value == text) {
                w.string(text);
                taken = true;
            }
        }
        break;
    }
    if (taken) {
        *json = w.text();
        raw->clear();
        return kSettingSet;
    }
    json->clear();
    *raw = settingRaw(text);
    return kSettingByHand;
}

const char* checkSettingValue(const ManifestSetting& option, const json::Value& value, std::string* line) {
    switch (option.type) {
    case kSettingBool:
        if (!value.isBool()) return "wrong-type";
        *line = value.boolean() ? "1" : "0";
        return 0;
    case kSettingInt:
        if (!value.isInteger()) return "wrong-type";
        if (value.integer() < option.min || value.integer() > option.max) return "out-of-range";
        *line = decimal(value.integer());
        return 0;
    case kSettingText: {
        if (!value.isString()) return "wrong-type";
        const std::string& text = value.string();
        if (text.empty() || !validUtf8(text)) return "bad-characters";
        if (text.size() > option.maxLength) return "too-long";
        if (!validSettingText(text, option.maxLength, option.anyCharacters)) return "bad-characters";
        *line = text;
        return 0;
    }
    case kSettingSelect:
        if (!value.isString()) return "wrong-type";
        for (size_t i = 0; i < option.values.size(); ++i) {
            if (option.values[i].value == value.string()) {
                *line = value.string();
                return 0;
            }
        }
        return "not-a-choice";
    }
    return "wrong-type";
}

std::string settingJson(const ManifestSetting& option, const SettingsFile& file) {
    json::Writer w;
    w.beginObject().key("key").string(option.key).key("label").string(option.label);
    if (!option.description.empty()) w.key("description").string(option.description);
    w.key("type").string(settingTypeName(option.type));
    if (option.type == kSettingInt) {
        w.key("min").integer(option.min).key("max").integer(option.max);
        if (!option.unit.empty()) w.key("unit").string(option.unit);
    } else if (option.type == kSettingText) {
        w.key("maxLength").integer((int64_t)option.maxLength).key("characters").string(option.anyCharacters ? "any" : "safe");
    } else if (option.type == kSettingSelect) {
        w.key("values").beginArray();
        for (size_t i = 0; i < option.values.size(); ++i) {
            w.beginObject().key("value").string(option.values[i].value).key("label").string(option.values[i].label).endObject();
        }
        w.endArray();
    }
    if (option.hasDefault) {
        w.key("default");
        if (option.type == kSettingBool) w.boolean(option.defaultBool);
        else if (option.type == kSettingInt) w.integer(option.defaultInt);
        else w.string(option.defaultText);
    }
    if (!option.unset.empty()) w.key("unset").string(option.unset);
    w.key("applies").string(option.applies).key("advanced").boolean(option.advanced);
    std::string text, value, raw;
    SettingState state = kSettingUnset;
    if (file.value(option.key, &text)) state = readSettingValue(option, text, &value, &raw);
    w.key("state").string(settingStateName(state));
    if (state == kSettingSet) w.key("value").raw(value);
    if (state == kSettingByHand) w.key("raw").string(raw);
    w.endObject();
    return w.text();
}

// ---- the file -----------------------------------------------------------------------------------------------------

namespace {

const char kDataPrefix[] = "${data}/";

// The folders on the way to the file, the data folder first, and the file. False if settings.file is not of the
// form the manifest checks, as a record from before the field was checked might hold.
bool locate(const Manifest& m, const Platform& platform, std::vector<std::string>* dirs, std::string* file, std::string* error) {
    if (!validSettingsFile(m.settingsFile)) {
        *error = "the manifest of " + m.id + " names no settings file the agent can use";
        return false;
    }
    std::string path = platform.dataDir(m.id);
    dirs->clear();
    dirs->push_back(path);
    std::string rest = m.settingsFile.substr(sizeof kDataPrefix - 1);
    for (size_t slash = rest.find('/'); slash != std::string::npos; slash = rest.find('/')) {
        path += "/" + rest.substr(0, slash);
        dirs->push_back(path);
        rest.erase(0, slash + 1);
    }
    *file = path + "/" + rest;
    return true;
}

std::string notAFolder(const std::string& path, fs::Kind kind) {
    return path + " is " + (kind == fs::kLink ? "a link" : "not a folder") + ", and the settings file is not looked for through it";
}

std::string notAFile(const std::string& path, fs::Kind kind) {
    return path + " is " + (kind == fs::kLink ? "a link" : kind == fs::kDir ? "a folder" : "not a regular file") +
           ", not the settings file";
}

}  // namespace

std::string settingsPath(const Manifest& m, const Platform& platform) {
    std::vector<std::string> dirs;
    std::string file, why;
    return locate(m, platform, &dirs, &file, &why) ? file : std::string();
}

bool readSettingsFile(const Manifest& m, const Platform& platform, std::string* text, bool* exists, std::string* error) {
    text->clear();
    *exists = false;
    std::vector<std::string> dirs;
    std::string file;
    if (!locate(m, platform, &dirs, &file, error)) return false;
    for (size_t i = 0; i < dirs.size(); ++i) {
        fs::Kind kind = fs::kindOf(dirs[i]);
        if (kind == fs::kNone) return true;
        if (kind != fs::kDir) {
            *error = notAFolder(dirs[i], kind);
            return false;
        }
    }
    fs::Kind kind = fs::kindOf(file);
    if (kind == fs::kNone) return true;
    if (kind != fs::kFile) {
        *error = notAFile(file, kind);
        return false;
    }
    LogFile named;
    named.path = file;
    named.real = file;
    int fd = -1;
    uint64_t size = 0;
    if (!openLogFile(named, &fd, &size, error)) return false;
    char buffer[16384];
    bool ok = true;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            *error = file + " cannot be read: " + strerror(errno);
            ok = false;
            break;
        }
        if (n == 0) break;
        if (text->size() + (size_t)n > kMaxSettingsFileBytes) {
            *error = file + " is larger than 64 KiB, which a settings file may not be";
            ok = false;
            break;
        }
        text->append(buffer, (size_t)n);
    }
    close(fd);
    if (!ok) {
        text->clear();
        return false;
    }
    *exists = true;
    return true;
}

bool writeSettingsFile(const Manifest& m, const Platform& platform, const std::string& text, std::string* error) {
    if (text.size() > kMaxSettingsFileBytes) {
        *error = "the settings file would be larger than 64 KiB, which a settings file may not be";
        return false;
    }
    std::vector<std::string> dirs;
    std::string file, why;
    if (!locate(m, platform, &dirs, &file, error)) return false;
    for (size_t i = 0; i < dirs.size(); ++i) {
        fs::Kind kind = fs::kindOf(dirs[i]);
        if (kind == fs::kNone) {
            // One folder at a time, each looked at again once it is made: whatever took its name meanwhile is not
            // gone through. The data folder's own parents are the platform's.
            bool made = i == 0 ? fs::makeDirs(dirs[i], &why) : fs::makeDir(dirs[i], &why);
            if (!made && !why.empty()) {
                *error = why;
                return false;
            }
            kind = fs::kindOf(dirs[i]);
        }
        if (kind != fs::kDir) {
            *error = notAFolder(dirs[i], kind);
            return false;
        }
    }
    unsigned mode = 0644;
    struct stat st;
    if (lstat(file.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            *error = notAFile(file, fs::kindOf(file));
            return false;
        }
        mode = (unsigned)(st.st_mode & 07777);
    } else if (errno != ENOENT) {
        *error = file + " cannot be looked at: " + strerror(errno);
        return false;
    }
    return fs::writeFile(file, text, mode, error);
}

}  // namespace nbt

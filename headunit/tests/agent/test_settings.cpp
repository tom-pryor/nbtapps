// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/settings.cpp: the value rules of package settings, as spec/settings.vectors has them, which
// the phone app's tests read too; and where the settings file is taken from and written to on a folder that
// stands in for a head unit, and where it is not (links, other kinds of file, a file too large).
//
// usage: test_settings [spec/settings.vectors] [work folder]     (default build/agent-host/work)

#include "../../agent/settings.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <string>

#include "../../agent/fsutil.hpp"
#include "../../agent/host_platform.hpp"
#include "../../agent/json.hpp"
#include "../../agent/json_text.hpp"
#include "../../agent/settings_file.hpp"
#include "test_util.hpp"

using namespace nbt;

namespace {

std::string num(long long value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    return text;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// A field of bytes: "=" and then the bytes, each %HH or itself.
bool bytesField(const std::string& field, std::string* out) {
    if (field.empty() || field[0] != '=') return false;
    out->clear();
    for (size_t i = 1; i < field.size(); ++i) {
        char c = field[i];
        if (c != '%') {
            if (c < 0x21 || c > 0x7e) return false;
            *out += c;
            continue;
        }
        if (i + 2 >= field.size() || hexDigit(field[i + 1]) < 0 || hexDigit(field[i + 2]) < 0) return false;
        *out += (char)(hexDigit(field[i + 1]) * 16 + hexDigit(field[i + 2]));
        i += 2;
    }
    return true;
}

// JSON as the reader of a value has it: parsed inside {"v": ...}, and written out again compact.
bool canonical(const std::string& text, std::string* out) {
    json::Document doc;
    if (!doc.parse("{\"v\":" + text + "}", 0)) return false;
    *out = jsonText(doc.root().find("v"));
    return true;
}

void put(const std::string& path, const std::string& text, unsigned mode = 0644) {
    std::string why;
    CHECK_IN(why, fs::makeDirs(fs::parentOf(path), &why) && fs::writeFile(path, text, mode, &why));
}

Manifest withFile(const char* id, const char* file) {
    Manifest m;
    m.id = id;
    m.version = "1.0.0";
    m.hasSettings = true;
    m.settingsFile = file;
    return m;
}

// What readSettingsFile says: "absent", "file:" and the text, or "refused".
std::string readBack(const Manifest& m, const Platform& platform) {
    std::string text, why;
    bool exists = true;
    if (!readSettingsFile(m, platform, &text, &exists, &why)) {
        CHECK_IN(m.settingsFile, !why.empty() && text.empty() && !exists);
        return "refused";
    }
    return exists ? "file:" + text : "absent";
}

unsigned modeOf(const std::string& path) {
    struct stat st;
    CHECK_IN(path, lstat(path.c_str(), &st) == 0);
    return (unsigned)(st.st_mode & 07777);
}

// Where the file is, and where it is not taken from.
void testPlaces(const std::string& work) {
    std::string root = work + "/places", why;
    CHECK_IN(why, fs::removeTree(root, &why));
    HostPlatform platform(root);
    CHECK_IN(why, platform.prepare(&why));
    const char* id = "org.nbtapp.dial";
    std::string data = platform.dataDir(id);
    Manifest dial = withFile(id, "${data}/dial.conf");
    CHECK(settingsPath(dial, platform) == data + "/dial.conf");

    // No data folder, no file: nothing set, and nothing refused. The first write makes both, 0644.
    CHECK(readBack(dial, platform) == "absent");
    CHECK_IN(why, writeSettingsFile(dial, platform, "A=1\n", &why));
    CHECK(readBack(dial, platform) == "file:A=1\n" && modeOf(data + "/dial.conf") == 0644);
    // A file there keeps its permission bits, and no temporary file is left beside it.
    CHECK(chmod((data + "/dial.conf").c_str(), 0600) == 0);
    CHECK_IN(why, writeSettingsFile(dial, platform, "A=2\n", &why));
    CHECK(readBack(dial, platform) == "file:A=2\n" && modeOf(data + "/dial.conf") == 0600);
    CHECK(fs::kindOf(data + "/" + fs::kTempName) == fs::kNone);
    // An empty file is a file.
    CHECK_IN(why, writeSettingsFile(dial, platform, "", &why));
    CHECK(readBack(dial, platform) == "file:");

    // Folders below the data folder: missing ones mean no file, and a write makes them.
    Manifest deep = withFile(id, "${data}/conf/sub/settings");
    CHECK(settingsPath(deep, platform) == data + "/conf/sub/settings");
    CHECK(readBack(deep, platform) == "absent");
    CHECK_IN(why, writeSettingsFile(deep, platform, "B=1\n", &why));
    CHECK(fs::kindOf(data + "/conf/sub") == fs::kDir && readBack(deep, platform) == "file:B=1\n");

    // A link on the way, even to a folder of the package's own, is not gone through, for reading or writing; and
    // nothing is written where it leads.
    std::string elsewhere = root + "/elsewhere";
    CHECK_IN(why, fs::makeDirs(elsewhere, &why));
    put(elsewhere + "/settings", "SECRET=1\n");
    CHECK_IN(why, fs::removeTree(data + "/conf", &why) && fs::makeDirs(data + "/conf", &why));
    CHECK(fs::makeLink(elsewhere, data + "/conf/sub", &why));
    CHECK(readBack(deep, platform) == "refused");
    CHECK(!writeSettingsFile(deep, platform, "B=2\n", &why) && !why.empty());
    CHECK(testutil::readFile(elsewhere + "/settings") == "SECRET=1\n" && fs::kindOf(elsewhere + "/" + fs::kTempName) == fs::kNone);
    CHECK(fs::removeFile(data + "/conf/sub", &why) && fs::makeLink("..", data + "/conf/sub", &why));
    CHECK(readBack(deep, platform) == "refused" && !writeSettingsFile(deep, platform, "B=2\n", &why));
    // A file in a folder's place.
    CHECK(fs::removeFile(data + "/conf/sub", &why));
    put(data + "/conf/sub", "not a folder\n");
    CHECK(readBack(deep, platform) == "refused" && !writeSettingsFile(deep, platform, "B=2\n", &why));
    CHECK(testutil::readFile(data + "/conf/sub") == "not a folder\n");

    // The file itself a link, a folder or a FIFO.
    Manifest other = withFile(id, "${data}/other.conf");
    CHECK(fs::makeLink(elsewhere + "/settings", data + "/other.conf", &why));
    CHECK(readBack(other, platform) == "refused" && !writeSettingsFile(other, platform, "C=1\n", &why));
    CHECK(fs::kindOf(data + "/other.conf") == fs::kLink && testutil::readFile(elsewhere + "/settings") == "SECRET=1\n");
    CHECK(fs::removeFile(data + "/other.conf", &why) && fs::makeDirs(data + "/other.conf", &why));
    CHECK(readBack(other, platform) == "refused" && !writeSettingsFile(other, platform, "C=1\n", &why));
    CHECK(fs::removeTree(data + "/other.conf", &why));
    CHECK(mkfifo((data + "/other.conf").c_str(), 0600) == 0);
    CHECK(readBack(other, platform) == "refused" && !writeSettingsFile(other, platform, "C=1\n", &why));
    CHECK(fs::removeFile(data + "/other.conf", &why));

    // The data folder itself a link.
    const char* linked = "org.nbtapp.linked";
    CHECK(fs::makeLink(elsewhere, platform.dataDir(linked), &why));
    Manifest viaLink = withFile(linked, "${data}/settings");
    CHECK(readBack(viaLink, platform) == "refused" && !writeSettingsFile(viaLink, platform, "D=1\n", &why));
    CHECK(testutil::readFile(elsewhere + "/settings") == "SECRET=1\n");

    // 64 KiB at most, read or written.
    std::string big(kMaxSettingsFileBytes, '#');
    CHECK_IN(why, writeSettingsFile(dial, platform, big, &why));
    CHECK(readBack(dial, platform) == "file:" + big);
    CHECK(!writeSettingsFile(dial, platform, big + "\n", &why) && !why.empty());
    put(data + "/dial.conf", big + "\n");
    CHECK(readBack(dial, platform) == "refused");

    // A settings.file of a form the manifest would refuse, as an old record might hold, names no file.
    Manifest odd = withFile(id, "${data}/../state/phones.json");
    CHECK(settingsPath(odd, platform).empty() && readBack(odd, platform) == "refused" && !writeSettingsFile(odd, platform, "", &why));
    CHECK_IN(why, fs::removeTree(root, &why));
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = argc > 1 ? argv[1] : "spec/settings.vectors";
    std::string work = std::string(argc > 2 ? argv[2] : "build/agent-host/work") + "/settings";
    char here[4096];
    CHECK(getcwd(here, sizeof here) != 0);
    if (work[0] != '/') work = std::string(here) + "/" + work;
    std::string text = testutil::readFile(path);
    std::map<std::string, ManifestSetting> options;
    size_t reads = 0, writes = 0, at = 0;
    int number = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        CHECK_IN(path + ": the last line has no line end", end != std::string::npos);
        std::string line = text.substr(at, end - at);
        at = end + 1;
        ++number;
        std::string where = path + ":" + num(number) + ": " + line.substr(0, 120);
        if (line.empty() || line[0] == '#') continue;
        CHECK_IN(where, line.find('\r') == std::string::npos && line.find('\t') == std::string::npos);
        size_t space = line.find(' ');
        CHECK_IN(where, space != std::string::npos);
        std::string kind = line.substr(0, space), rest = line.substr(space + 1);

        if (kind == "option") {
            // Into a manifest of its own, read as any manifest is.
            std::string manifest = "{\"manifestVersion\":1,\"id\":\"org.nbtapp.vectors\",\"name\":\"Vectors\",\"version\":\"1.0.0\","
                                   "\"contents\":[],\"settings\":{\"options\":[" + rest + "]}}";
            Manifest m;
            Error error;
            CHECK_IN(where + ": " + error.message, parseManifest(manifest.data(), manifest.size(), &m, &error));
            CHECK_IN(where, m.settings.size() == 1 && options.count(m.settings[0].key) == 0);
            options[m.settings[0].key] = m.settings[0];
            continue;
        }

        // KEY, then a bytes field or a reason, then the rest.
        size_t second = rest.find(' ');
        CHECK_IN(where, second != std::string::npos);
        std::string key = rest.substr(0, second);
        rest = rest.substr(second + 1);
        CHECK_IN(where + ": no option " + key, options.count(key) == 1);
        const ManifestSetting& option = options[key];
        size_t third = rest.find(' ');
        CHECK_IN(where, third != std::string::npos);
        std::string first = rest.substr(0, third), last = rest.substr(third + 1);

        if (kind == "read") {
            std::string value, json, raw;
            CHECK_IN(where, bytesField(first, &value));
            SettingState state = readSettingValue(option, value, &json, &raw);
            if (last.compare(0, 4, "set ") == 0) {
                std::string expected;
                CHECK_IN(where, canonical(last.substr(4), &expected));
                CHECK_IN(where + "\n  got: " + settingStateName(state) + " " + json + raw,
                         state == kSettingSet && json == expected && raw.empty());
            } else {
                CHECK_IN(where, last.compare(0, 8, "by-hand ") == 0);
                std::string expected;
                CHECK_IN(where, bytesField(last.substr(8), &expected));
                CHECK_IN(where + "\n  got: " + settingStateName(state) + " " + json + raw,
                         state == kSettingByHand && raw == expected && json.empty());
            }
            ++reads;
        } else {
            CHECK_IN(where, kind == "write");
            json::Document doc;
            CHECK_IN(where, doc.parse("{\"v\":" + last + "}", 0));
            std::string written;
            const char* reason = checkSettingValue(option, doc.root().find("v"), &written);
            if (first[0] == '=') {
                std::string expected;
                CHECK_IN(where, bytesField(first, &expected));
                CHECK_IN(where + "\n  got: " + (reason != 0 ? reason : written), reason == 0 && written == expected);
            } else {
                CHECK_IN(where + "\n  got: " + (reason != 0 ? reason : "=" + written), reason != 0 && first == reason);
            }
            ++writes;
        }
    }
    // A file cut short or emptied by mistake would pass as well: it has at least these.
    CHECK(options.size() >= 7 && reads >= 60 && writes >= 40);

    // Values the vectors cannot hold: what is not inside the JSON profile never reaches a check, but a string of
    // bytes that is not UTF-8 is refused all the same, and a NUL is a control character.
    {
        ManifestSetting any = options["TA"];
        std::string written;
        CHECK(readSettingValue(any, std::string("a\0b", 3), &written, &written) == kSettingByHand);
        // Bytes that only continue a character: the cut moves back three at most, and each byte is one U+FFFD.
        std::string replaced;
        for (int i = 0; i < 197; ++i) replaced += "\xEF\xBF\xBD";
        CHECK(settingRaw(std::string(300, '\x80')) == replaced);
        CHECK(settingStateName(kSettingUnset) == std::string("unset") && settingStateName(kSettingSet) == std::string("set") &&
              settingStateName(kSettingByHand) == std::string("by-hand"));
    }

    testPlaces(work);

    printf("test_settings: ok (%u options, %u reads, %u writes)\n", (unsigned)options.size(), (unsigned)reads, (unsigned)writes);
    return 0;
}

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/manifest.cpp: what a good manifest is read into, each string form of the schema at its edges,
// the code a refusal carries, and the risk flags. The deliberately broken packages (test_package) cover the same
// checker from the outside; this test reaches the cases they do not.

#include "../../agent/json_writer.hpp"
#include "../../agent/manifest.hpp"
#include "../../agent/utf8.hpp"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

using namespace nbt;

static const char kHash[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// A manifest with the given text in place of the usual members: `head` after manifestVersion, `contents` and
// `install` as the bodies of those two.
static std::string make(const std::string& head, const std::string& contents, const std::string& install) {
    return "{\"manifestVersion\":1," + head + "\"contents\":[" + contents + "]" +
           (install.empty() ? "" : ",\"install\":{" + install + "}") + "}";
}

static std::string content(const std::string& path) {
    return "{\"path\":\"" + path + "\",\"size\":3,\"sha256\":\"" + kHash + "\"}";
}

static const char kHead[] = "\"id\":\"org.nbtapp.t\",\"name\":\"T\",\"version\":\"1.0.0\",";

// "" when the text is accepted, otherwise the code of the refusal.
static std::string code(const std::string& text, Manifest* out = 0) {
    Manifest m;
    Error error;
    bool ok = parseManifest(text.data(), text.size(), &m, &error);
    assert(ok == error.code.empty() && (ok || !error.message.empty()));
    if (out != 0) *out = m;
    return error.code;
}

// One case of spec/log-paths.vectors.
struct LogPathCase {
    int line;
    std::string verdict;   // ok, skip or bad
    std::string path;      // with each %HH made the byte HH
};

static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static std::vector<LogPathCase> logPathCases(const char* file) {
    std::vector<LogPathCase> cases;
    FILE* f = fopen(file, "rb");
    if (f == 0) {
        fprintf(stderr, "test_manifest: cannot open %s\n", file);
        exit(1);
    }
    std::string line;
    int number = 0;
    for (;;) {
        line.clear();
        int c;
        while ((c = fgetc(f)) != EOF && c != '\n') line += (char)c;
        if (c == EOF && line.empty()) break;
        ++number;
        if (line.empty() || line[0] == '#') continue;
        size_t note = line.find("  #");
        if (note != std::string::npos) line.erase(note);
        size_t space = line.find(' ');
        LogPathCase one;
        one.line = number;
        one.verdict = line.substr(0, space);
        assert(one.verdict == "ok" || one.verdict == "skip" || one.verdict == "bad");
        std::string text = space == std::string::npos ? "" : line.substr(space + 1);
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%') {
                assert(i + 2 < text.size() && hexDigit(text[i + 1]) >= 0 && hexDigit(text[i + 2]) >= 0);
                one.path += (char)(hexDigit(text[i + 1]) * 16 + hexDigit(text[i + 2]));
                i += 2;
            } else {
                one.path += text[i];
            }
        }
        cases.push_back(one);
    }
    fclose(f);
    return cases;
}

// The code for a manifest whose `member` (of the top level, of a file entry, ...) is given as JSON text.
static std::string head(const std::string& members) { return code(make(members, "", "")); }
static std::string file(const std::string& members) {
    return code(make(kHead, content("a/b"), "\"files\":[{" + members + "}]"));
}
static std::string dest(const std::string& value) {
    return file("\"src\":\"a/b\",\"dest\":\"" + value + "\",\"mode\":\"0644\"");
}
static std::string link(const std::string& path, const std::string& target) {
    return code(make(kHead, "", "\"links\":[{\"path\":\"" + path + "\",\"target\":\"" + target + "\"}]"));
}
static std::string withId(const std::string& id) { return head("\"id\":\"" + id + "\",\"name\":\"T\",\"version\":\"1.0.0\","); }
static std::string withName(const std::string& name) {
    return head("\"id\":\"org.nbtapp.t\",\"name\":\"" + name + "\",\"version\":\"1.0.0\",");
}

int main() {
    const std::string bad = "bad-manifest", unsupported = "unsupported-manifest";

    // 1. A full manifest, and what it is read into.
    std::string full = make(
        std::string(kHead) + "\"requires\":{\"agent\":\">=1.0.0\"},"
        "\"depends\":[{\"id\":\"org.nbtapp.runtime\",\"version\":\">=2.0.0 <3.0.0\"},{\"id\":\"org.nbtapp.any\"}],"
        "\"conflicts\":[{\"id\":\"org.other.thing\",\"version\":\"<1.0.0\"}],",
        content("payload/bin/tool") + "," + content("payload/lib/b.so") + "," + content("payload/lib/a.so") + "," +
            content("payload/lib/sub/c.txt") + "," + content("payload/menu.xml") + "," + content("scripts/pre.sh"),
        "\"files\":["
        "{\"src\":\"payload/bin/tool\",\"dest\":\"${app}/bin/tool\",\"mode\":\"0755\"},"
        "{\"src\":\"payload/lib/\",\"dest\":\"${plugins}/Demo/\",\"mode\":\"0644\"},"
        "{\"src\":\"payload/menu.xml\",\"dest\":\"/opt/hmi/menu.xml\",\"mode\":\"0640\",\"config\":false,"
        "\"replaces\":{\"sha256\":[\"" + std::string(kHash) + "\"]}}],"
        "\"links\":[{\"path\":\"${app}/bin/t\",\"target\":\"tool\"}],"
        "\"hooks\":{\"preInstall\":{\"script\":\"scripts/pre.sh\",\"timeout\":600}},"
        "\"reboot\":\"hmi-restart\","
        "\"serialServices\":[{\"name\":\"Test_Radio-1\",\"program\":\"radio.d+x\"},{\"program\":\"radiod\",\"name\":\"R\"}]");
    Manifest m;
    assert(code(full, &m) == "");
    assert(m.id == "org.nbtapp.t" && m.name == "T" && m.version == "1.0.0" && m.agentRange == ">=1.0.0");
    assert(m.depends.size() == 2 && m.depends[0].id == "org.nbtapp.runtime" && m.depends[0].range == ">=2.0.0 <3.0.0");
    assert(m.depends[1].id == "org.nbtapp.any" && m.depends[1].range == "");
    assert(m.conflicts.size() == 1 && m.conflicts[0].range == "<1.0.0");
    assert(m.contents.size() == 6 && m.contents[1].path == "payload/lib/b.so" && m.contents[1].size == 3);
    assert(m.files.size() == 3 && m.files[0].mode == 0755 && m.files[2].mode == 0640);
    assert(!m.files[0].hasConfig && m.files[2].hasConfig && !m.files[2].config);
    assert(m.files[2].hasReplaces && m.files[2].replaces.size() == 1 && m.files[2].replaces[0] == kHash);
    assert(m.links.size() == 1 && m.links[0].target == "tool");
    assert(m.hasHooks && m.hooks[kPreInstall].present && m.hooks[kPreInstall].timeout == 600);
    assert(m.hooks[kPreInstall].script == "scripts/pre.sh" && !m.hooks[kPostInstall].present);
    assert(m.reboot == "hmi-restart");
    assert(m.serialServices.size() == 2 && m.serialServices[0].name == "Test_Radio-1" && m.serialServices[0].program == "radio.d+x");
    assert(m.serialServices[1].name == "R" && m.serialServices[1].program == "radiod");
    // Placements: the folder entry gives one per file under it, in the order of the paths.
    assert(m.placements.size() == 5);
    assert(m.placements[0].src == "payload/bin/tool" && m.placements[0].dest == "${app}/bin/tool" && m.placements[0].file == 0);
    assert(m.placements[1].src == "payload/lib/a.so" && m.placements[1].dest == "${plugins}/Demo/a.so");
    assert(m.placements[1].file == 1 && m.placements[1].content == 2);
    assert(m.placements[2].dest == "${plugins}/Demo/b.so" && m.placements[3].dest == "${plugins}/Demo/sub/c.txt");
    assert(m.placements[4].dest == "/opt/hmi/menu.xml" && m.placements[4].file == 2 && m.placements[4].content == 4);
    std::vector<std::string> flags = riskFlags(m);
    assert(flags.size() == 3 && flags[0] == "runs-scripts" && flags[1] == "modifies-system" && flags[2] == "needs-reboot");

    // The least a manifest can be.
    assert(code(make(kHead, "", ""), &m) == "");
    assert(m.reboot == "none" && !m.hasHooks && m.placements.empty() && riskFlags(m).empty() && m.serialServices.empty());

    // 2. The top level: the JSON profile, manifestVersion, the required members.
    assert(code("") == bad && code("[]") == bad && code("{\"manifestVersion\":1,}") == bad);
    assert(code("{}") == bad && code("{\"manifestVersion\":\"1\"}") == bad && code("{\"manifestVersion\":true}") == bad);
    assert(code("{\"manifestVersion\":2}") == unsupported && code("{\"manifestVersion\":0}") == unsupported);
    assert(code("{\"manifestVersion\":1}") == bad);
    assert(head("\"id\":\"org.nbtapp.t\",\"name\":\"T\",") == bad);                      // no version
    assert(code("{\"manifestVersion\":1," + std::string(kHead) + "\"contents\":{}}") == bad);
    assert(code("{\"manifestVersion\":1,\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1.0.0\"}") == bad);   // no contents
    // Unknown keys at the top, in a dependency and in a content entry are ignored.
    assert(head(std::string(kHead) + "\"future\":{\"x\":[1,2]},\"depends\":[{\"id\":\"a.b\",\"note\":1}],") == "");
    assert(code(make(kHead, "{\"path\":\"a\",\"size\":0,\"sha256\":\"" + std::string(kHash) + "\",\"x\":null}", "")) == "");
    // Inside requires, install, a file, replaces, a link, hooks and a hook they are refused as unsupported.
    assert(head(std::string(kHead) + "\"requires\":{\"firmware\":\">=1.0.0\"},") == unsupported);
    assert(code(make(kHead, "", "\"later\":1")) == unsupported);
    assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0644\",\"patch\":\"bsdiff\"") == unsupported);
    assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0644\",\"replaces\":{\"sha256\":[\"" + std::string(kHash) +
                "\"],\"size\":1}") == unsupported);
    assert(code(make(kHead, "", "\"links\":[{\"path\":\"/x\",\"target\":\"y\",\"z\":1}]")) == unsupported);
    assert(code(make(kHead, content("s"), "\"hooks\":{\"onBoot\":{\"script\":\"s\",\"timeout\":1}}")) == unsupported);
    assert(code(make(kHead, content("s"), "\"hooks\":{\"preInstall\":{\"script\":\"s\",\"timeout\":1,\"user\":\"x\"}}")) == unsupported);
    // The first fault in the order of the text decides: a required member that is missing comes before any key.
    assert(file("\"patch\":\"bsdiff\",\"src\":\"a/b\",\"dest\":\"/x\"") == bad);                     // no mode
    assert(file("\"mode\":\"644\",\"src\":\"a/b\",\"dest\":\"/x\",\"patch\":1") == bad);             // the bad mode is first
    assert(file("\"patch\":1,\"mode\":\"644\",\"src\":\"a/b\",\"dest\":\"/x\"") == unsupported);     // the unknown key is first

    // 3. Types.
    assert(head(std::string(kHead) + "\"requires\":[],") == bad && head(std::string(kHead) + "\"depends\":{},") == bad);
    assert(head(std::string(kHead) + "\"depends\":[\"a.b\"],") == bad && head(std::string(kHead) + "\"depends\":[{}],") == bad);
    assert(head(std::string(kHead) + "\"requires\":{\"agent\":1},") == bad);
    assert(code(make(kHead, "\"a\"", "")) == bad && code(make(kHead, "{\"path\":\"a\",\"size\":\"3\",\"sha256\":\"" + std::string(kHash) + "\"}", "")) == bad);
    assert(code(make(kHead, "", "\"files\":{}")) == bad && code(make(kHead, "", "\"links\":{}")) == bad);
    assert(code(make(kHead, "", "\"hooks\":[]")) == bad && code(make(kHead, "", "\"reboot\":1")) == bad);
    assert(code(make(kHead, "", "\"reboot\":\"maybe\"")) == bad && code(make(kHead, "", "\"reboot\":\"required\"")) == "");
    assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":644") == bad);
    assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0644\",\"config\":1") == bad);
    assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0644\",\"replaces\":{\"sha256\":[]}") == bad);
    assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0644\",\"replaces\":{}") == bad);
    assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0644\",\"replaces\":{\"sha256\":\"" + std::string(kHash) + "\"}") == bad);

    // 4. Package ids.
    const char* const ids[] = {"a.b", "org.nbtapp.hello", "a-b.c-d.e", "0.1", "a1.b2-c3", 0};
    for (int i = 0; ids[i] != 0; ++i) assert(withId(ids[i]) == "");
    const char* const noIds[] = {"", "hello", "Org.nbtapp.hello", "a..b", ".a.b", "a.b.", "-a.b", "a-.b", "a.-b", "a.b-",
                                 "a--b.c", "a_b.c", "a b.c", "a.b\\n", "\\u00e9.b", 0};
    for (int i = 0; noIds[i] != 0; ++i) assert(withId(noIds[i]) == bad);
    assert(withId(std::string(126, 'a') + ".b") == "" && withId(std::string(127, 'a') + ".b") == bad);

    // 5. Names: 1 to 64 characters, counted as characters, with no control character.
    assert(withName("") == bad && withName("x") == "" && withName(std::string(64, 'x')) == "");
    assert(withName(std::string(65, 'x')) == bad);
    std::string accents;
    for (int i = 0; i < 64; ++i) accents += "\xc3\xa9";   // 64 characters in 128 bytes
    assert(utf8Length(accents) == 64 && validUtf8(accents) && !validUtf8("\xc3") && !validUtf8("\xed\xa0\x80"));
    assert(withName(accents) == "" && withName(accents + "x") == bad);
    assert(withName("a\\u0000b") == bad && withName("a\\tb") == bad && withName("a\\u007fb") == bad);
    assert(withName("a\\u0080b") == "" && withName("\\ud83d\\ude00") == "");

    // 6. Versions, ranges, hashes and sizes in place.
    assert(head("\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1.2\",") == bad);
    assert(head("\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1.2.0-rc.1+b\",") == "");
    assert(head("\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1.0.0-" + std::string(58, 'a') + "\",") == "");
    assert(head("\"id\":\"a.b\",\"name\":\"T\",\"version\":\"1.0.0-" + std::string(59, 'a') + "\",") == bad);
    assert(head(std::string(kHead) + "\"requires\":{\"agent\":\">=1.0\"},") == bad);
    assert(head(std::string(kHead) + "\"conflicts\":[{\"id\":\"a.b\",\"version\":\"\"}],") == bad);
    std::string upper = kHash;
    upper[10] = 'A';
    assert(code(make(kHead, "{\"path\":\"a\",\"size\":0,\"sha256\":\"" + upper + "\"}", "")) == bad);
    assert(code(make(kHead, "{\"path\":\"a\",\"size\":0,\"sha256\":\"" + std::string(kHash).substr(1) + "\"}", "")) == bad);
    assert(code(make(kHead, "{\"path\":\"a\",\"size\":-1,\"sha256\":\"" + std::string(kHash) + "\"}", "")) == bad);
    assert(code(make(kHead, "{\"path\":\"a\",\"size\":4294967294,\"sha256\":\"" + std::string(kHash) + "\"}", "")) == "");
    assert(code(make(kHead, "{\"path\":\"a\",\"size\":4294967295,\"sha256\":\"" + std::string(kHash) + "\"}", "")) == bad);

    // 7. Paths in contents.
    const char* const paths[] = {"a", "a/b", "a.b/c..d", "...", "a/.b", "a b", "caf\\u00e9/x", "a/\\u007f", 0};
    for (int i = 0; paths[i] != 0; ++i) assert(code(make(kHead, content(paths[i]), "")) == "");
    const char* const noPaths[] = {"", "/a", "a/", "a//b", ".", "..", "a/./b", "a/../b", "a/..", "./a", "a\\\\b", "a\\nb",
                                   "a/\\u0000", "manifest.json", "manifest.sig", 0};
    for (int i = 0; noPaths[i] != 0; ++i) assert(code(make(kHead, content(noPaths[i]), "")) == bad);
    assert(code(make(kHead, content("a") + "," + content("a"), "")) == bad);                      // twice
    assert(code(make(kHead, content(std::string(255, 'p')), "")) == "" && code(make(kHead, content(std::string(256, 'p')), "")) == bad);
    std::string wide;                                                                             // 128 characters, 256 bytes
    for (int i = 0; i < 128; ++i) wide += "\xc3\xa9";
    assert(code(make(kHead, content(wide), "")) == bad);

    // 8. Destinations.
    const char* const dests[] = {"${app}/b", "${data}/x/y", "${plugins}/Demo/b", "/x", "/opt/a.b/c", "/${app}/x",
                                 "${app}/${data}", 0};
    for (int i = 0; dests[i] != 0; ++i) assert(dest(dests[i]) == "");
    const char* const noDests[] = {"", "x", "bin/x", "${app}", "${home}/x", "${APP}/x", "${app}//x", "${app}/../x",
                                   "${app}/x/./y", "/", "//x", "/x//y", "/x/../y", "/..", "${app}x", "$app/x",
                                   "/x\\\\y", "/x\\ty", "${app}/", "/x/", "${plugins}/Demo/", 0};
    for (int i = 0; noDests[i] != 0; ++i) assert(dest(noDests[i]) == bad);   // the last three: a file to a folder
    // A folder goes to a folder, which may be the top of ${app}.
    std::string folder = "\"files\":[{\"src\":\"a/\",\"dest\":\"${app}/\",\"mode\":\"0644\"}]";
    assert(code(make(kHead, content("a/b") + "," + content("a/c/d"), folder), &m) == "");
    assert(m.placements.size() == 2 && m.placements[0].dest == "${app}/b" && m.placements[1].dest == "${app}/c/d");
    assert(code(make(kHead, content("a/b"), "\"files\":[{\"src\":\"a/\",\"dest\":\"/opt/x\",\"mode\":\"0644\"}]")) == bad);
    assert(code(make(kHead, content("b/b"), folder)) == bad);                                    // nothing under a/
    assert(code(make(kHead, content("a/b"), "\"files\":[{\"src\":\"a/\",\"dest\":\"/x/\",\"mode\":\"0644\",\"config\":false}]")) == bad);
    assert(code(make(kHead, content("ab"), folder)) == bad);                                     // "ab" is not under "a/"
    // No destination twice: file and file, folder and file, file and link.
    assert(code(make(kHead, content("a/b") + "," + content("c"),
                     "\"files\":[{\"src\":\"a/\",\"dest\":\"/x/\",\"mode\":\"0644\"},{\"src\":\"c\",\"dest\":\"/x/b\",\"mode\":\"0644\"}]")) == bad);
    assert(code(make(kHead, content("a/b"),
                     "\"files\":[{\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0644\"}],\"links\":[{\"path\":\"/x\",\"target\":\"y\"}]")) == bad);
    assert(code(make(kHead, "", "\"links\":[{\"path\":\"/x\",\"target\":\"y\"},{\"path\":\"/x\",\"target\":\"z\"}]")) == bad);
    // A placed path over 255 bytes, reached only by expanding a folder.
    assert(code(make(kHead, content("a/" + std::string(200, 'f')),
                     "\"files\":[{\"src\":\"a/\",\"dest\":\"/" + std::string(100, 'd') + "/\",\"mode\":\"0644\"}]")) == bad);
    assert(file("\"src\":\"a/c\",\"dest\":\"/x\",\"mode\":\"0644\"") == bad);                     // src not in contents

    // 9. Modes.
    const char* const noModes[] = {"", "644", "0888", "00644", "0x1f", "075", "07555", "-644", 0};
    for (int i = 0; noModes[i] != 0; ++i) assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"" + std::string(noModes[i]) + "\"") == bad);
    assert(file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0000\"") == "" && file("\"src\":\"a/b\",\"dest\":\"/x\",\"mode\":\"0777\"") == "");

    // 10. Links.
    assert(link("${app}/bin/x", "y") == "" && link("/usr/bin/x", "../lib/y") == "" && link("${plugins}/A/b", "/abs") == "");
    const char* const noLinks[] = {"", "x", "${app}", "${app}/", "/x/", "/", "${app}x/y", "${home}/x", "/x/../y", 0};
    for (int i = 0; noLinks[i] != 0; ++i) assert(link(noLinks[i], "y") == bad);
    assert(link("/x", "") == bad && link("/x", "a\\nb") == bad && link("/x", std::string(255, 't')) == "");
    assert(link("/x", std::string(256, 't')) == bad);

    // 11. Hooks.
    assert(code(make(kHead, content("s"), "\"hooks\":{}"), &m) == "" && !m.hasHooks && riskFlags(m).empty());
    assert(code(make(kHead, content("s"), "\"hooks\":{\"postRemove\":{\"script\":\"s\",\"timeout\":1}}"), &m) == "");
    assert(m.hasHooks && m.hooks[kPostRemove].present && riskFlags(m).size() == 1);
    assert(code(make(kHead, content("s"), "\"hooks\":{\"postRemove\":{\"script\":\"s\",\"timeout\":0}}")) == bad);
    assert(code(make(kHead, content("s"), "\"hooks\":{\"postRemove\":{\"script\":\"s\",\"timeout\":601}}")) == bad);
    assert(code(make(kHead, content("s"), "\"hooks\":{\"postRemove\":{\"script\":\"t\",\"timeout\":5}}")) == bad);
    assert(code(make(kHead, content("s"), "\"hooks\":{\"postRemove\":{\"script\":\"s\"}}")) == bad);

    // 11a. Serial services: a name the Bluetooth stack takes and that is not taken, a program's file name, and no
    //      name twice.
    {
        struct Services {
            static std::string code(const std::string& list, Manifest* out = 0) {
                return ::code(make(kHead, "", "\"serialServices\":" + list), out);
            }
            static std::string one(const std::string& name, const std::string& program) {
                return code("[{\"name\":\"" + name + "\",\"program\":\"" + program + "\"}]");
            }
        };
        assert(Services::code("[]") == "" && Services::one("TestRadio", "radiod") == "");
        assert(Services::one("a", "b") == "" && Services::one(std::string(32, 'n'), std::string(64, 'p')) == "");
        assert(Services::one(std::string(33, 'n'), "p") == bad && Services::one("n", std::string(65, 'p')) == bad);
        const char* const noNames[] = {"", "Test Radio", "Test.Radio", "Test/Radio", "caf\\u00e9", "q'", "NBTApp", "SPP", 0};
        for (int i = 0; noNames[i] != 0; ++i) assert(Services::one(noNames[i], "radiod") == bad);
        // Only the taken names themselves are refused, in their own case.
        assert(Services::one("NBTApp2", "radiod") == "" && Services::one("nbtapp", "radiod") == "" && Services::one("spp", "radiod") == "");
        const char* const noPrograms[] = {"", ".", "..", "bin/radiod", "/radiod", "radio d", "radiod\\n", "caf\\u00e9", 0};
        for (int i = 0; noPrograms[i] != 0; ++i) assert(Services::one("TestRadio", noPrograms[i]) == bad);
        assert(Services::one("TestRadio", "...") == "" && Services::one("TestRadio", ".radiod") == "" &&
               Services::one("TestRadio", "radio-d_2.0+x") == "");
        assert(Services::code("{}") == bad && Services::code("[\"TestRadio\"]") == bad && Services::code("[{}]") == bad);
        assert(Services::code("[{\"name\":\"TestRadio\"}]") == bad && Services::code("[{\"program\":\"radiod\"}]") == bad);
        assert(Services::code("[{\"name\":7,\"program\":\"radiod\"}]") == bad);
        assert(Services::code("[{\"name\":\"TestRadio\",\"program\":\"radiod\",\"uuid\":\"x\"}]") == unsupported);
        // Twice in one manifest, whatever the programs; two names for one program are fine.
        assert(Services::code("[{\"name\":\"A\",\"program\":\"x\"},{\"name\":\"A\",\"program\":\"y\"}]") == bad);
        assert(Services::code("[{\"name\":\"A\",\"program\":\"x\"},{\"name\":\"B\",\"program\":\"x\"}]", &m) == "");
        assert(m.serialServices.size() == 2 && riskFlags(m).empty());
        assert(validSerialServiceName("TestRadio") && !validSerialServiceName("NBTApp") && !validSerialServiceName("SPP"));
        assert(validProgramName("radiod") && !validProgramName("bin/radiod") && !validProgramName(".."));
    }

    // 11b. Logs: each path of spec/log-paths.vectors (ok and skip taken, bad refused), at most 16 entries, none
    //      twice. An entry is open, and old agents skip the key: a top-level key they do not know is ignored.
    {
        struct Logs {
            static std::string code(const std::string& list, Manifest* out = 0) {
                return ::code(make(std::string(kHead) + "\"logs\":" + list + ",", "", ""), out);
            }
            static std::string one(const std::string& path) { return code("[{\"path\":\"" + path + "\"}]"); }
        };
        assert(Logs::code("[]", &m) == "" && m.logs.empty());
        assert(Logs::code("[{\"path\":\"${data}/log/*.log\"},{\"path\":\"/tmp/radio.log\",\"label\":\"x\"}]", &m) == "");
        assert(m.logs.size() == 2 && m.logs[0] == "${data}/log/*.log" && m.logs[1] == "/tmp/radio.log" && riskFlags(m).empty());
        // Each path of spec/log-paths.vectors, which nbtpkg.py and the phone app are held to as well, in a manifest
        // and alone: a skipped one is taken in a manifest and is not a path the agent lists.
        std::vector<LogPathCase> cases = logPathCases("spec/log-paths.vectors");
        assert(cases.size() > 40);
        for (size_t i = 0; i < cases.size(); ++i) {
            json::Writer w;
            w.beginArray().beginObject().key("path").string(cases[i].path).endObject().endArray();
            const std::string& verdict = cases[i].verdict;
            LogPathForm form = verdict == "ok" ? kLogPathOk : verdict == "skip" ? kLogPathSkip : kLogPathBad;
            if (Logs::code(w.text()) != (form == kLogPathBad ? bad : "") || logPathForm(cases[i].path) != form ||
                validLogPath(cases[i].path) != (form == kLogPathOk)) {
                fprintf(stderr, "test_manifest: spec/log-paths.vectors line %d: %s is not %s\n", cases[i].line,
                        cases[i].path.c_str(), cases[i].verdict.c_str());
                return 1;
            }
        }
        // 255 characters, not bytes, as for a destination.
        std::string longest = "${data}/" + std::string(247, 'x');
        assert(longest.size() == 255 && Logs::one(longest) == "" && Logs::one(longest + "x") == bad);
        std::string accents = "${data}/";
        for (int i = 0; i < 247; ++i) accents += "\\u00e9";
        assert(Logs::one(accents) == "");
        std::string sixteen = "[", seventeen;
        for (int i = 0; i < 16; ++i) sixteen += std::string(i ? "," : "") + "{\"path\":\"/tmp/" + std::string(1, (char)('a' + i)) + "\"}";
        seventeen = sixteen + ",{\"path\":\"/tmp/q\"}]";
        sixteen += "]";
        assert(Logs::code(sixteen, &m) == "" && m.logs.size() == 16 && Logs::code(seventeen) == bad);
        assert(Logs::code("{}") == bad && Logs::code("[\"/tmp/x\"]") == bad && Logs::code("[{}]") == bad);
        assert(Logs::code("[{\"path\":7}]") == bad && Logs::code("[{\"file\":\"/tmp/x\"}]") == bad);
        assert(Logs::code("[{\"path\":\"/tmp/x\"},{\"path\":\"/tmp/x\"}]") == bad);
        assert(Logs::code("[{\"path\":\"/tmp/x\"},{\"path\":\"/tmp/y\"}]") == "");
    }

    // 11c. Settings: options of four types, each held to its own definition and to the rules that span its fields;
    //      an option of another type is skipped, with only its key and label looked at. The object and each option
    //      are open, and old agents skip the key.
    {
        struct Settings {
            static std::string code(const std::string& settings, Manifest* out = 0, const std::string& install = "") {
                return ::code(make(std::string(kHead) + "\"settings\":" + settings + ",", "", install), out);
            }
            // One option with these members.
            static std::string one(const std::string& members, Manifest* out = 0) {
                return code("{\"options\":[{" + members + "}]}", out);
            }
            static std::string lamp(const std::string& more, Manifest* out = 0) {
                return one("\"key\":\"LAMP\",\"label\":\"Lamp\"," + more, out);
            }
        };
        std::string every =
            "{\"file\":\"${data}/dial.conf\",\"note\":1,\"options\":["
            "{\"key\":\"CHIME\",\"label\":\"Chime\",\"description\":\"On or off.\",\"type\":\"bool\",\"default\":true,"
            "\"applies\":\"app-start\",\"advanced\":true,\"min\":\"not looked at\"},"
            "{\"key\":\"SPEED\",\"label\":\"Speed\",\"type\":\"int\",\"min\":-2147483648,\"max\":2147483647,"
            "\"unit\":\"\\u00b0/s\",\"default\":-5,\"x\":[1]},"
            "{\"key\":\"COLOUR\",\"label\":\"Colour\",\"type\":\"colour\",\"default\":{},\"description\":7},"
            "{\"key\":\"GREETING\",\"label\":\"Greeting\",\"type\":\"text\",\"maxLength\":6,\"characters\":\"any\","
            "\"default\":\"\\u00e9 \\u00e9!\",\"applies\":\"reboot\"},"
            "{\"key\":\"HOST\",\"label\":\"Host\",\"type\":\"text\",\"maxLength\":255,\"unset\":\"No lamp\"},"
            "{\"key\":\"FACE\",\"label\":\"Face\",\"type\":\"select\",\"values\":[{\"value\":\"round\",\"label\":\"Round\"},"
            "{\"value\":\"sq-2.x_y\",\"label\":\"Square\",\"z\":0}],\"default\":\"sq-2.x_y\",\"applies\":\"hmi-restart\"}]}";
        assert(Settings::code(every, &m) == "");
        assert(m.hasSettings && m.settingsFile == "${data}/dial.conf" && m.settings.size() == 5 && riskFlags(m).empty());
        const ManifestSetting& chime = m.settings[0];
        assert(chime.key == "CHIME" && chime.label == "Chime" && chime.description == "On or off." && chime.type == kSettingBool);
        assert(chime.hasDefault && chime.defaultBool && chime.applies == "app-start" && chime.advanced && chime.unset.empty());
        const ManifestSetting& speed = m.settings[1];
        assert(speed.type == kSettingInt && speed.min == -2147483648LL && speed.max == 2147483647LL);
        assert(speed.unit == "\xc2\xb0/s" && speed.hasDefault && speed.defaultInt == -5 && speed.applies == "now" && !speed.advanced);
        const ManifestSetting& greeting = m.settings[2];   // COLOUR, of an unknown type, is left out
        assert(greeting.key == "GREETING" && greeting.type == kSettingText && greeting.maxLength == 6 && greeting.anyCharacters);
        assert(greeting.hasDefault && greeting.defaultText == "\xc3\xa9 \xc3\xa9!" && greeting.applies == "reboot");
        const ManifestSetting& host = m.settings[3];
        assert(host.type == kSettingText && host.maxLength == 255 && !host.anyCharacters && !host.hasDefault && host.unset == "No lamp");
        const ManifestSetting& face = m.settings[4];
        assert(face.type == kSettingSelect && face.values.size() == 2 && face.values[1].value == "sq-2.x_y");
        assert(face.values[1].label == "Square" && face.hasDefault && face.defaultText == "sq-2.x_y");
        assert(std::string(settingTypeName(face.type)) == "select" && std::string(settingTypeName(kSettingInt)) == "int");
        // No file named: the default one. No settings at all: none.
        assert(Settings::lamp("\"type\":\"bool\"", &m) == "" && m.hasSettings && m.settingsFile == "${data}/settings");
        assert(m.settings.size() == 1 && !m.settings[0].hasDefault);
        assert(code(make(kHead, "", ""), &m) == "" && !m.hasSettings && m.settings.empty());
        // Only options of unknown types: the manifest is good and the package has no setting.
        assert(Settings::lamp("\"type\":\"colour\"", &m) == "" && m.hasSettings && m.settings.empty());

        // The object and the list.
        assert(Settings::code("[]") == bad && Settings::code("{}") == bad && Settings::code("{\"options\":[]}") == bad);
        assert(Settings::code("{\"options\":{}}") == bad && Settings::code("{\"options\":[\"LAMP\"]}") == bad);
        std::string many = "{\"options\":[";
        for (int i = 0; i < 64; ++i) {
            char key[16];
            snprintf(key, sizeof key, "L%d", i);
            many += std::string(i ? "," : "") + "{\"key\":\"" + key + "\",\"label\":\"L\",\"type\":\"bool\"}";
        }
        assert(Settings::code(many + "]}", &m) == "" && m.settings.size() == 64);
        assert(Settings::code(many + ",{\"key\":\"L64\",\"label\":\"L\",\"type\":\"x\"}]}") == bad);
        // The file: under ${data} only, a file and not a folder, the segment rules of every path, 255 characters.
        const char* const files[] = {"${data}/a", "${data}/a/b.conf", "${data}/.x", "${data}/a b", 0};
        for (int i = 0; files[i] != 0; ++i) assert(validSettingsFile(files[i]));
        const char* const noFiles[] = {"", "${data}", "${data}/", "${data}/a/", "${data}//a", "${data}/../a", "${data}/./a",
                                       "${app}/a", "${tmp}/a", "/tmp/a", "a", "${data}/a\\b", "${data}/a\nb", 0};
        for (int i = 0; noFiles[i] != 0; ++i) assert(!validSettingsFile(noFiles[i]));
        assert(validSettingsFile("${data}/" + std::string(247, 'f')) && !validSettingsFile("${data}/" + std::string(248, 'f')));
        assert(Settings::code("{\"file\":\"${app}/s\",\"options\":[{\"key\":\"A\",\"label\":\"A\",\"type\":\"bool\"}]}") == bad);
        assert(Settings::code("{\"file\":7,\"options\":[{\"key\":\"A\",\"label\":\"A\",\"type\":\"bool\"}]}") == bad);

        // What every option has: a key, a label and a type, also on an option of an unknown type.
        assert(Settings::one("\"label\":\"L\",\"type\":\"bool\"") == bad && Settings::one("\"key\":\"L\",\"type\":\"bool\"") == bad);
        assert(Settings::one("\"key\":\"L\",\"label\":\"L\"") == bad && Settings::lamp("\"type\":1") == bad);
        const char* const keys[] = {"A", "A_1", "Z9", "NBT_VID_FPS", 0};
        for (int i = 0; keys[i] != 0; ++i) assert(Settings::one("\"key\":\"" + std::string(keys[i]) + "\",\"label\":\"L\",\"type\":\"bool\"") == "");
        const char* const noKeys[] = {"", "a", "Lamp", "1A", "_A", "A-B", "A B", "\\u00c9", 0};
        for (int i = 0; noKeys[i] != 0; ++i) {
            assert(Settings::one("\"key\":\"" + std::string(noKeys[i]) + "\",\"label\":\"L\",\"type\":\"bool\"") == bad);
            assert(Settings::one("\"key\":\"" + std::string(noKeys[i]) + "\",\"label\":\"L\",\"type\":\"colour\"") == bad);
        }
        assert(Settings::one("\"key\":\"" + std::string(64, 'K') + "\",\"label\":\"L\",\"type\":\"bool\"") == "");
        assert(Settings::one("\"key\":\"" + std::string(65, 'K') + "\",\"label\":\"L\",\"type\":\"bool\"") == bad);
        assert(Settings::one("\"key\":\"A\",\"label\":\"\",\"type\":\"colour\"") == bad);
        assert(Settings::one("\"key\":\"A\",\"label\":\"a\\tb\",\"type\":\"bool\"") == bad);
        assert(Settings::one("\"key\":\"A\",\"label\":\"" + std::string(65, 'l') + "\",\"type\":\"bool\"") == bad);
        // No key twice, counting the options that are skipped.
        assert(Settings::code("{\"options\":[{\"key\":\"A\",\"label\":\"A\",\"type\":\"bool\"},{\"key\":\"A\",\"label\":\"B\",\"type\":\"int\",\"min\":0,\"max\":1}]}") == bad);
        assert(Settings::code("{\"options\":[{\"key\":\"A\",\"label\":\"A\",\"type\":\"bool\"},{\"key\":\"A\",\"label\":\"B\",\"type\":\"x\"}]}") == bad);
        // An unknown type has nothing else looked at.
        assert(Settings::lamp("\"type\":\"BOOL\",\"default\":\"x\",\"unset\":\"\",\"applies\":\"later\",\"description\":[1]") == "");

        // Members of every known type.
        assert(Settings::lamp("\"type\":\"bool\",\"description\":\"" + std::string(500, 'd') + "\"") == "");
        assert(Settings::lamp("\"type\":\"bool\",\"description\":\"" + std::string(501, 'd') + "\"") == bad);
        assert(Settings::lamp("\"type\":\"bool\",\"description\":\"\"") == "");
        assert(Settings::lamp("\"type\":\"bool\",\"description\":\"a\\nb\"") == bad);
        assert(Settings::lamp("\"type\":\"bool\",\"unset\":\"\"") == bad && Settings::lamp("\"type\":\"bool\",\"unset\":\"Off\"") == "");
        assert(Settings::lamp("\"type\":\"bool\",\"unset\":\"Off\",\"default\":false") == bad);
        assert(Settings::lamp("\"type\":\"bool\",\"default\":false,\"unset\":\"Off\"") == bad);
        const char* const applies[] = {"now", "app-start", "hmi-restart", "reboot", 0};
        for (int i = 0; applies[i] != 0; ++i) {
            assert(Settings::lamp("\"type\":\"bool\",\"applies\":\"" + std::string(applies[i]) + "\"", &m) == "");
            assert(m.settings[0].applies == applies[i]);
        }
        assert(Settings::lamp("\"type\":\"bool\",\"applies\":\"later\"") == bad && Settings::lamp("\"type\":\"bool\",\"applies\":1") == bad);
        assert(Settings::lamp("\"type\":\"bool\",\"advanced\":\"yes\"") == bad && Settings::lamp("\"type\":\"bool\",\"advanced\":false") == "");
        // bool.
        assert(Settings::lamp("\"type\":\"bool\",\"default\":\"true\"") == bad && Settings::lamp("\"type\":\"bool\",\"default\":1") == bad);
        assert(Settings::lamp("\"type\":\"bool\",\"default\":false", &m) == "" && m.settings[0].hasDefault && !m.settings[0].defaultBool);
        // int: the limits, a unit, a default inside the limits.
        assert(Settings::lamp("\"type\":\"int\",\"max\":1") == bad && Settings::lamp("\"type\":\"int\",\"min\":1") == bad);
        assert(Settings::lamp("\"type\":\"int\",\"min\":2,\"max\":1") == bad && Settings::lamp("\"type\":\"int\",\"min\":1,\"max\":1") == "");
        assert(Settings::lamp("\"type\":\"int\",\"min\":-2147483649,\"max\":1") == bad);
        assert(Settings::lamp("\"type\":\"int\",\"min\":0,\"max\":2147483648") == bad);
        assert(Settings::lamp("\"type\":\"int\",\"min\":\"0\",\"max\":1") == bad);
        assert(Settings::lamp("\"type\":\"int\",\"min\":0,\"max\":9,\"default\":9") == "");
        assert(Settings::lamp("\"type\":\"int\",\"min\":0,\"max\":9,\"default\":10") == bad);
        assert(Settings::lamp("\"type\":\"int\",\"min\":0,\"max\":9,\"default\":-1") == bad);
        assert(Settings::lamp("\"type\":\"int\",\"min\":0,\"max\":9,\"default\":\"1\"") == bad);
        assert(Settings::lamp("\"type\":\"int\",\"min\":0,\"max\":9,\"unit\":\"\"") == bad);
        assert(Settings::lamp("\"type\":\"int\",\"min\":0,\"max\":9,\"unit\":\"" + std::string(16, 'u') + "\"") == "");
        assert(Settings::lamp("\"type\":\"int\",\"min\":0,\"max\":9,\"unit\":\"" + std::string(17, 'u') + "\"") == bad);
        assert(Settings::lamp("\"type\":\"bool\",\"unit\":\"\",\"min\":\"x\"") == "");     // an int's keys on a bool
        // text: maxLength, the two sets of characters, a default it takes.
        assert(Settings::lamp("\"type\":\"text\"") == bad && Settings::lamp("\"type\":\"text\",\"maxLength\":0") == bad);
        assert(Settings::lamp("\"type\":\"text\",\"maxLength\":255") == "" && Settings::lamp("\"type\":\"text\",\"maxLength\":256") == bad);
        assert(Settings::lamp("\"type\":\"text\",\"maxLength\":9,\"characters\":\"ascii\"") == bad);
        assert(Settings::lamp("\"type\":\"text\",\"maxLength\":9,\"characters\":\"safe\",\"default\":\"a.b_c:d/e@f+g,h-1\"") == bad);   // 17 bytes
        assert(Settings::lamp("\"type\":\"text\",\"maxLength\":17,\"characters\":\"safe\",\"default\":\"a.b_c:d/e@f+g,h-1\"") == "");
        assert(Settings::lamp("\"type\":\"text\",\"maxLength\":9,\"default\":\"\"") == bad);
        assert(Settings::lamp("\"type\":\"text\",\"maxLength\":9,\"default\":5") == bad);
        assert(Settings::lamp("\"type\":\"text\",\"maxLength\":5,\"characters\":\"any\",\"default\":\"\\u00e9\\u00e9\\u00e9\"") == bad);
        assert(Settings::lamp("\"type\":\"text\",\"maxLength\":6,\"characters\":\"any\",\"default\":\"\\u00e9\\u00e9\\u00e9\"") == "");
        const char* const safe[] = {"a", "Z9", "lamp-1.example:80/x@y+z,w_v", 0};
        const char* const unsafe[] = {"", "a b", "a=b", "a\"b", "a#b", "\xc3\xa9", "a\tb", "a;b", 0};
        const char* const any[] = {"a b", "a=b;c#d", "\xc3\xa9 x", "!", 0};
        const char* const noAny[] = {"", " a", "a ", " ", "a\tb", "a\nb", "a\x7f" "b", 0};
        for (int i = 0; safe[i] != 0; ++i) assert(validSettingText(safe[i], 255, false) && validSettingText(safe[i], 255, true));
        for (int i = 0; unsafe[i] != 0; ++i) assert(!validSettingText(unsafe[i], 255, false));
        for (int i = 0; any[i] != 0; ++i) assert(validSettingText(any[i], 255, true));
        for (int i = 0; noAny[i] != 0; ++i) assert(!validSettingText(noAny[i], 255, true));
        assert(validSettingText("abc", 3, false) && !validSettingText("abcd", 3, false) && !validSettingText("\xc3\xa9\xc3\xa9", 3, true));
        // select: 2 to 16 values, each once and of its form, a default among them.
        std::string two = "\"type\":\"select\",\"values\":[{\"value\":\"a\",\"label\":\"A\"},{\"value\":\"b\",\"label\":\"B\"}]";
        assert(Settings::lamp(two) == "" && Settings::lamp(two + ",\"default\":\"b\"") == "");
        assert(Settings::lamp(two + ",\"default\":\"c\"") == bad && Settings::lamp(two + ",\"default\":\"\"") == bad);
        assert(Settings::lamp("\"type\":\"select\"") == bad && Settings::lamp("\"type\":\"select\",\"values\":{}") == bad);
        assert(Settings::lamp("\"type\":\"select\",\"values\":[{\"value\":\"a\",\"label\":\"A\"}]") == bad);
        assert(Settings::lamp("\"type\":\"select\",\"values\":[{\"value\":\"a\",\"label\":\"A\"},{\"value\":\"a\",\"label\":\"B\"}]") == bad);
        assert(Settings::lamp("\"type\":\"select\",\"values\":[{\"value\":\"a\",\"label\":\"A\"},{\"value\":\"b\"}]") == bad);
        assert(Settings::lamp("\"type\":\"select\",\"values\":[{\"value\":\"a\",\"label\":\"A\"},{\"value\":\"b b\",\"label\":\"B\"}]") == bad);
        assert(Settings::lamp("\"type\":\"select\",\"values\":[{\"value\":\"a\",\"label\":\"A\"},\"b\"]") == bad);
        std::string sixteen = "\"type\":\"select\",\"values\":[";
        for (int i = 0; i < 16; ++i) sixteen += std::string(i ? "," : "") + "{\"value\":\"v" + std::string(1, (char)('a' + i)) + "\",\"label\":\"V\"}";
        assert(Settings::lamp(sixteen + "]", &m) == "" && m.settings[0].values.size() == 16);
        assert(Settings::lamp(sixteen + ",{\"value\":\"vq\",\"label\":\"V\"}]") == bad);
        assert(Settings::lamp("\"type\":\"select\",\"values\":[{\"value\":\"" + std::string(64, 'v') + "\",\"label\":\"A\"},{\"value\":\"b\",\"label\":\"B\"}]") == "");
        assert(Settings::lamp("\"type\":\"select\",\"values\":[{\"value\":\"" + std::string(65, 'v') + "\",\"label\":\"A\"},{\"value\":\"b\",\"label\":\"B\"}]") == bad);

        // At most 1,024 bytes of UTF-8 in an option's strings: 4 + 4 bytes of key and label here, and the description
        // in characters of two bytes, which counts bytes and not characters.
        std::string accents;
        for (int i = 0; i < 508; ++i) accents += "\\u00e9";
        assert(Settings::lamp("\"type\":\"bool\",\"description\":\"" + accents.substr(0, 6 * 500) + "\"") == "");   // 1,008
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(20, 'L') + "\",\"type\":\"bool\","
                             "\"description\":\"" + accents.substr(0, 6 * 500) + "\"") == "");   // 1,024
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(21, 'L') + "\",\"type\":\"bool\","
                             "\"description\":\"" + accents.substr(0, 6 * 500) + "\"") == bad);  // 1,025
        // The unset text, an int's unit and a select's values count; members a type does not have do not.
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(21, 'L') + "\",\"type\":\"bool\",\"unset\":\"U\","
                             "\"description\":\"" + accents.substr(0, 6 * 499) + "\"") == "");   // 1,024
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(22, 'L') + "\",\"type\":\"bool\",\"unset\":\"U\","
                             "\"description\":\"" + accents.substr(0, 6 * 499) + "\"") == bad);
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(21, 'L') + "\",\"type\":\"int\",\"min\":0,\"max\":1,"
                             "\"unit\":\"U\",\"description\":\"" + accents.substr(0, 6 * 499) + "\"") == "");
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(22, 'L') + "\",\"type\":\"int\",\"min\":0,\"max\":1,"
                             "\"unit\":\"U\",\"description\":\"" + accents.substr(0, 6 * 499) + "\"") == bad);
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(22, 'L') + "\",\"type\":\"bool\",\"unit\":\"U\","
                             "\"description\":\"" + accents.substr(0, 6 * 499) + "\"") == "");
        std::string bulky = "\"type\":\"select\",\"values\":[";   // 16 values of 61 characters and their labels: 992
        for (int i = 0; i < 16; ++i) {
            char value[8];
            snprintf(value, sizeof value, "%02d", i);
            bulky += std::string(i ? "," : "") + "{\"value\":\"" + value + std::string(59, 'v') + "\",\"label\":\"V\"}";
        }
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(28, 'L') + "\"," + bulky + "]") == "");    // 1,024
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(29, 'L') + "\"," + bulky + "]") == bad);   // 1,025
        assert(Settings::one("\"key\":\"LAMP\",\"label\":\"" + std::string(27, 'L') + "\"," + bulky + "],\"default\":\"00" +
                             std::string(59, 'v') + "\"") == bad);   // the default counts too: 1,084

        // Where the code comes from: the walk names the first fault of the text; the rules of an option's type come
        // after it, as nbtpkg.py checks them, so a key unknown in install decides even when it comes later.
        assert(Settings::code("{\"options\":[{\"key\":\"a\",\"label\":\"A\",\"type\":\"bool\"}]}", 0, "\"later\":1") == bad);
        assert(Settings::code("{\"options\":[{\"key\":\"A\",\"label\":\"A\",\"type\":\"bool\",\"default\":1}]}", 0, "\"later\":1") ==
               unsupported);
    }

    // 12. Risk flags: a destination outside the package's own places, or a `replaces`, is modifies-system.
    assert(code(make(kHead, content("a/b"), "\"files\":[{\"src\":\"a/b\",\"dest\":\"${data}/x\",\"mode\":\"0644\"}]"), &m) == "");
    assert(riskFlags(m).empty());
    assert(code(make(kHead, content("a/b"), "\"files\":[{\"src\":\"a/b\",\"dest\":\"${plugins}/A/x\",\"mode\":\"0644\","
                     "\"replaces\":{\"sha256\":[\"" + std::string(kHash) + "\"]}}]"), &m) == "");
    assert(riskFlags(m).size() == 1 && riskFlags(m)[0] == "modifies-system");
    assert(code(make(kHead, "", "\"links\":[{\"path\":\"/bin/x\",\"target\":\"y\"}]"), &m) == "");
    assert(riskFlags(m).size() == 1 && riskFlags(m)[0] == "modifies-system");
    assert(inOwnDirs("${app}/x") && inOwnDirs("${data}/") && !inOwnDirs("${app}") && !inOwnDirs("/x") && !inOwnDirs("${home}/x"));

    // 13. The size limit.
    std::string large = "{\"manifestVersion\":1,\"pad\":\"" + std::string(kMaxManifestBytes, 'x') + "\"}";
    assert(code(large) == bad);

    // 14. Read for an agent of a version: a requires.agent that leaves it out is agent-too-old before anything else
    // of the manifest is read, so that a later format behind it is not called bad by an agent that is too old for it.
    {
        struct For {
            static std::string code(const std::string& text, const char* agentVersion) {
                Manifest m;
                Error error;
                bool ok = parseManifest(text.data(), text.size(), &m, &error, agentVersion);
                assert(ok == error.code.empty() && (ok || !error.message.empty()));
                return error.code;
            }
        };
        // A key this agent does not know in requires, and settings of a later format.
        const std::string later = make(kHead + std::string("\"requires\":{\"later\":true,\"agent\":\">=2.0.0\"},"
                                       "\"settings\":{\"options\":[{\"key\":\"LAMP\",\"label\":\"Lamp\",\"type\":\"bool\",\"applies\":\"later\"}]},"),
                                       "", "");
        assert(For::code(later, "1.9.9") == "agent-too-old");
        assert(For::code(later, "2.0.0-rc.1") == "agent-too-old");   // a pre-release is older than its release
        Manifest m;
        Error error;
        assert(!parseManifest(later.data(), later.size(), &m, &error, "1.4.1") && error.message == "needs agent >=2.0.0; this is 1.4.1");
        // An agent the range takes, or none, reads the rest as ever.
        assert(For::code(later, "2.0.0") == unsupported && For::code(later, "") == unsupported && code(later) == unsupported);
        // Only a requires.agent that is a range is read early; whatever else is wrong is the walk's to say.
        assert(For::code(make(kHead + std::string("\"requires\":{\"agent\":\">=2.0\"},"), "", ""), "1.0.0") == bad);
        assert(For::code(make(kHead + std::string("\"requires\":{\"agent\":2},"), "", ""), "1.0.0") == bad);
        assert(For::code(make(kHead + std::string("\"requires\":[\">=2.0.0\"],"), "", ""), "1.0.0") == bad);
        assert(For::code(make(kHead + std::string("\"requires\":{\"agent\":\">=1.0.0\"},"), "", ""), "1.0.0") == "");
        assert(For::code(make(kHead, "", ""), "0.0.1") == "");
        // Not before the JSON profile and manifestVersion.
        assert(For::code("{\"manifestVersion\":2,\"requires\":{\"agent\":\">=2.0.0\"}}", "1.0.0") == unsupported);
        assert(For::code("{\"manifestVersion\":1,\"requires\":{\"agent\":\">=2.0.0\",\"agent\":\">=2.0.0\"}}", "1.0.0") == bad);
        assert(For::code("[1]", "1.0.0") == bad);
    }

    printf("test_manifest: ok\n");
    return 0;
}

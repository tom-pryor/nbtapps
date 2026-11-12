// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/manifest.cpp: what a good manifest is read into, each string form of the schema at its edges,
// the code a refusal carries, and the risk flags. The deliberately broken packages (test_package) cover the same
// checker from the outside; this test reaches the cases they do not.

#include "../../agent/manifest.hpp"
#include "../../agent/utf8.hpp"

#include <assert.h>
#include <stdio.h>

#include <string>

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

    printf("test_manifest: ok\n");
    return 0;
}

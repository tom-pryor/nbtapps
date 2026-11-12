// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/hmi_ids.cpp: the ids are read from this repo's own plugin files (plugin/NbtApp.manifest and
// plugin/NbtApp.xml), and text that is not what the reader expects yields nothing instead of a wrong id.
//
// usage: test_hmi_ids [plugin folder]     (default plugin, as `make` runs it)

#include "../../agent/hmi_ids.hpp"

#include "test_util.hpp"

using namespace nbt;

static std::vector<long> connectors(const std::string& xml, std::string* desc = 0) {
    std::vector<long> ids;
    std::string descFile;
    readPluginManifest(xml, &ids, &descFile);
    if (desc != 0) *desc = descFile;
    return ids;
}

static std::vector<long> focus(const std::string& xml) {
    std::vector<long> ids;
    readPluginDescription(xml, &ids);
    return ids;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "plugin";

    // 1. The real files.
    std::string desc;
    std::vector<long> ids = connectors(testutil::readFile(dir + "/NbtApp.manifest"), &desc);
    CHECK(ids.size() == 1 && ids[0] == 211 && desc == "{pluginDir}/NbtApp.xml");
    ids = focus(testutil::readFile(dir + "/NbtApp.xml"));
    CHECK(ids.size() == 1 && ids[0] == 32754);

    // 2. The manifest: only an <id> directly inside <pluginAppConnector> counts.
    CHECK(connectors("<plugin><id>5</id><pluginAppConnector><id> 17 </id></pluginAppConnector></plugin>").size() == 1);
    CHECK(connectors("<plugin><pluginAppConnector><id>17</id></pluginAppConnector></plugin>")[0] == 17);
    ids = connectors("<a><pluginAppConnector><id>1</id></pluginAppConnector><pluginAppConnector><x><id>9</id></x><id>2</id>"
                     "</pluginAppConnector><DescFile>\n {pluginDir}/A.xml </DescFile><DescFile>second</DescFile></a>", &desc);
    CHECK(ids.size() == 2 && ids[0] == 1 && ids[1] == 2 && desc == "{pluginDir}/A.xml");
    // Comments and CDATA.
    CHECK(connectors("<pluginAppConnector><!-- <id>3</id> --><id><![CDATA[44]]></id></pluginAppConnector>")[0] == 44);
    CHECK(connectors("<!-- <pluginAppConnector><id>3</id></pluginAppConnector> -->").empty());
    // Not a number, or not closed: nothing.
    CHECK(connectors("<pluginAppConnector><id>x1</id><id>1 2</id><id></id><id>-4</id><id>1234567890</id></pluginAppConnector>").empty());
    CHECK(connectors("<pluginAppConnector><id>7").empty() && connectors("").empty() && connectors("<<<>>><id>").empty());
    CHECK(connectors("<pluginAppConnector><id>7</id>").size() == 1);   // the element that matters did close
    CHECK(connectors("plain text, no markup at all", &desc).empty() && desc.empty());

    // 3. The description: the attribute of every pluginApp, in either kind of quotes, and nowhere else.
    ids = focus("<pluginApps><pluginApp entryButtonFocusID=\"100\" x='1'/><pluginApp a=\"b\" entryButtonFocusID = '200'>"
                "<entryButton entryButtonFocusID=\"300\"/></pluginApp><other entryButtonFocusID=\"400\"/></pluginApps>");
    CHECK(ids.size() == 2 && ids[0] == 100 && ids[1] == 200);
    CHECK(focus("<!-- <pluginApp entryButtonFocusID=\"1\"/> --><pluginApp text=\"entryButtonFocusID=5\"/>").empty());
    CHECK(focus("<pluginApp entryButtonFocusID=\"\"/><pluginApp entryButtonFocusID=\"1x\"/><pluginApp entryButtonFocusID/>").empty());
    CHECK(focus("<pluginApp entryButtonFocusID=\"12").empty() && focus("<pluginApp").empty());

    // 4. The ids the HMI's own apps use.
    CHECK(builtInConnectorId(17) && builtInConnectorId(200) && !builtInConnectorId(211) && !builtInConnectorId(0));

    printf("test_hmi_ids: ok\n");
    return 0;
}

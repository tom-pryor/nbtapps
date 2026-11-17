// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/settings_file.cpp: which lines of a package's settings file are settings, the value a key
// has, and that set() and remove() change the lines of their key and leave every other byte as it was.

#include "../../agent/settings_file.hpp"

#include <stdio.h>

#include "test_util.hpp"

using namespace nbt;

static std::string valueOf(const SettingsFile& file, const char* key) {
    std::string value;
    return file.value(key, &value) ? value : std::string("<none>");
}

int main() {
    // 1. What is a setting, and what it says. A comment, an empty line, a line with no "=", a line that starts with a
    //    space and one of a key that is a longer key's start are not the key's lines.
    {
        SettingsFile f("# CHIME=0\n\nCHIME\n CHIME=0\nCHIMES=0\nCHIME=on\nOTHER=a=b\n");
        CHECK(valueOf(f, "CHIME") == "on" && f.count("CHIME") == 1 && valueOf(f, "CHIMES") == "0");
        CHECK(valueOf(f, "OTHER") == "a=b" && valueOf(f, "GONE") == "<none>" && valueOf(f, "") == "<none>");
    }
    // An empty value is a value; the last line of a key wins; a last line with no line end has its value too.
    {
        SettingsFile f("A=\nB=1\nB=2\nC=last");
        CHECK(valueOf(f, "A") == "" && valueOf(f, "B") == "2" && f.count("B") == 2 && valueOf(f, "C") == "last");
    }
    // A CR before the line end is not part of the value, and only one is taken off; spaces stay.
    {
        SettingsFile f("A=x\r\nB=y\r\r\nC= z \nD=w\r");
        CHECK(valueOf(f, "A") == "x" && valueOf(f, "B") == "y\r" && valueOf(f, "C") == " z " && valueOf(f, "D") == "w");
    }
    // Bytes that are not text are a value like any other.
    {
        SettingsFile f(std::string("A=\xff\x00\x01\n", 6));
        CHECK(valueOf(f, "A") == std::string("\xff\x00\x01", 3));
    }

    // 2. Nothing changed: the text comes back byte for byte, whatever its line ends.
    {
        const char* texts[] = {"", "\n", "A=1", "A=1\n", "# x\r\n\r\nA=1\r\n  B = 2\nno equals\n=\n", "\n\n\nA=1\n\n"};
        for (size_t i = 0; i < sizeof texts / sizeof texts[0]; ++i) CHECK_IN(texts[i], SettingsFile(texts[i]).text() == texts[i]);
    }

    // 3. set: in place of the key's first line, ending in "\n", and the key's later lines go. Every other line stays,
    //    the CRs of the lines it does not touch included.
    {
        SettingsFile f("# settings\r\nA=1\r\nB=2\r\nA=3\r\n# end\r\n");
        f.set("A", "9");
        CHECK_IN(f.text(), f.text() == "# settings\r\nA=9\nB=2\r\n# end\r\n" && valueOf(f, "A") == "9" && f.count("A") == 1);
        f.set("B", "");
        CHECK_IN(f.text(), f.text() == "# settings\r\nA=9\nB=\n# end\r\n");
    }
    // A new key at the end; after a line end where the file had none at its end.
    {
        SettingsFile f("A=1\n");
        f.set("B", "2");
        CHECK_IN(f.text(), f.text() == "A=1\nB=2\n");
        SettingsFile g("A=1");
        g.set("B", "2");
        CHECK_IN(g.text(), g.text() == "A=1\nB=2\n");
        SettingsFile h("# only a comment");
        h.set("A", "x");
        CHECK_IN(h.text(), h.text() == "# only a comment\nA=x\n");
        SettingsFile empty;
        empty.set("A", "x");
        CHECK_IN(empty.text(), empty.text() == "A=x\n");
    }
    // A key whose first line is the last line of the file, with no line end: that line gets one.
    {
        SettingsFile f("B=2\nA=1");
        f.set("A", "7");
        CHECK_IN(f.text(), f.text() == "B=2\nA=7\n");
    }
    // Lines that only look like the key's are left alone.
    {
        SettingsFile f("#A=1\n A=2\nAB=3\nA\n");
        f.set("A", "4");
        CHECK_IN(f.text(), f.text() == "#A=1\n A=2\nAB=3\nA\nA=4\n");
    }

    // 4. remove: every line of the key, and nothing else. A key with no line changes nothing.
    {
        SettingsFile f("A=1\r\n# x\nB=2\nA=3\nC=4");
        f.remove("A");
        CHECK_IN(f.text(), f.text() == "# x\nB=2\nC=4" && valueOf(f, "A") == "<none>");
        f.remove("NOPE");
        CHECK_IN(f.text(), f.text() == "# x\nB=2\nC=4");
        f.remove("C");
        CHECK_IN(f.text(), f.text() == "# x\nB=2\n");
        f.remove("B");
        f.set("D", "5");
        CHECK_IN(f.text(), f.text() == "# x\nD=5\n");
    }

    // 5. parse again: what was there before is gone.
    {
        SettingsFile f("A=1\n");
        f.parse("B=2\n");
        CHECK(valueOf(f, "A") == "<none>" && f.text() == "B=2\n");
    }

    printf("test_settings_file: ok\n");
    return 0;
}

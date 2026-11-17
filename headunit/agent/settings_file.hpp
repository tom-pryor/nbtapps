// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SETTINGS_FILE_HPP
#define NBT_AGENT_SETTINGS_FILE_HPP

// The text of a package's settings file (the manifest's settings.file), as the agent edits it. The file is the
// package's: its programs read it and may write it, and so may a person at a shell. So the agent keeps no copy of
// its own, and changes only the lines of the keys it is asked to, leaving every other byte as it was.
//
//   - A line ends at "\n"; the last one may have none. A setting's line starts at its first byte with KEY and "=",
//     and its value is the rest of the line, without the "\n" and without one "\r" before it. No quotes, no
//     escapes: the value is the bytes as they stand.
//   - Every other line is not a setting and is kept as it is: a comment (#), an empty line, a line with no "=", a
//     line that starts with a space, a line of a key nobody asked about.
//   - A key on more than one line has the value of the last.
//   - set() puts KEY=value in place of the first line of the key, ending in "\n", and drops the key's later lines;
//     a key that has no line gets one at the end of the file, after a "\n" if the file did not end in one.
//     remove() drops every line of the key.
//
// A file is at most kMaxSettingsFileBytes: the agent refuses to read a larger one, and to write one.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>

#include <string>
#include <vector>

namespace nbt {

const size_t kMaxSettingsFileBytes = 64 * 1024;

class SettingsFile {
public:
    SettingsFile() {}
    explicit SettingsFile(const std::string& text) { parse(text); }

    // Takes the text of a file in place of what was there.
    void parse(const std::string& text);

    // The value of the key's last line. False if no line has the key.
    bool value(const std::string& key, std::string* out) const;
    // The number of lines that have the key.
    size_t count(const std::string& key) const;
    void set(const std::string& key, const std::string& value);
    void remove(const std::string& key);

    // The file as it is now: the lines, each with its line end as it was or as set() wrote it.
    std::string text() const;

private:
    // Whether a line is one of the key's.
    static bool isLineOf(const std::string& line, const std::string& key);

    std::vector<std::string> lines_;   // each with its "\n", but the last one may have none
};

}  // namespace nbt

#endif

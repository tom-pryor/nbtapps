// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SETTINGS_HPP
#define NBT_AGENT_SETTINGS_HPP

// The settings of an installed package (the plan document, "Message reference", Settings): the options its
// manifest names in `settings` (manifest.hpp), whose values stand as lines KEY=value in one file in the package's
// data folder (settings_file.hpp). Here, the rules a value is held to, in both directions:
//
//   read   the value as the file has it: `set` when the option's type takes it, with the value settings.get
//          gives (a boolean for bool, an integer for int, a string for text and select); `by-hand` when it does
//          not, with the bytes as they stand as `raw`
//   check  a value a controller sends with settings.set: the text the agent writes after KEY=, or the reason it
//          is refused with
//
//   bool    reads 1, on, yes, true as true and 0, off, no, false as false, in any case; written as 1 or 0
//   int     reads -?[0-9]+ inside min..max, leading zeros included; written as its decimal digits, none of them
//           a leading zero
//   text    reads and takes valid UTF-8 that validSettingText() takes: 1 to maxLength bytes, of the safe set or,
//           with characters "any", with no control character and no space at either end
//   select  reads and takes one of the option's values, exactly
//
// The reasons of a refusal, as settings.set names them:
//
//   wrong-type      not the option's JSON type: a boolean, an integer, a string
//   out-of-range    an integer below min or above max
//   too-long        a text of more than maxLength bytes of UTF-8
//   bad-characters  a text that is empty, not UTF-8, or has a character its rule does not allow
//   not-a-choice    a string that is not one of a select option's values
//
// (unknown-key, a key the manifest names no option for, is the caller's.) spec/settings.vectors has cases of both
// directions, which the agent's tests and the phone app's read.
//
// Where the file is: the package's data folder (Platform::dataDir) and the part of settings.file after ${data}/.
// The package's programs, which run as root as the agent does, can put anything there, so the file is taken only
// where its text says it is:
//
//   - The data folder and every folder below it on the way to the file are real folders (lstat): a link, even one
//     to a folder, or any other kind of file there is refused.
//   - The file is a regular file, or not there at all. A link, a folder, a FIFO in its place is refused.
//   - It is opened as log_files.hpp opens a log: the file lstat saw, still a regular file, and the same one.
//   - A file of more than kMaxSettingsFileBytes (settings_file.hpp) is refused.
//
// A folder that is missing on the way means no file: every option is unset. A write makes the folders that are
// missing below the data folder (and the data folder, should it have gone), and puts a whole new file in place of
// the old (fs::writeFile: a temporary file, synced, renamed), with the old file's permission bits or 0644.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "json.hpp"
#include "manifest.hpp"
#include "platform.hpp"
#include "settings_file.hpp"

namespace nbt {

// The smallest frame a link may have for settings.get and settings.set: one option, whatever its strings, fits a
// reply on it. On a link with a smaller one they are answered `unsupported`.
const uint32_t kSettingsMinFrame = 4096;

// The most bytes of a value set by hand that settings.get gives as `raw`: the file's bytes are cut there, at the
// start of the character that would not fit whole. Fewer than a value's 255 (kSettingsMaxValueBytes): an option
// whose every string is at its limit and full of characters JSON escapes, with a raw of control characters (six
// bytes each in JSON), has to fit one reply on a frame of 4,096 bytes, and at 255 it would not
// (tests/agent/test_agent.cpp measures it).
const size_t kSettingsMaxRawBytes = 200;

// The words a bool reads (above), in any case: 1 for true, 0 for false, -1 for a word that is neither. nbtctl
// reads what it is given for a bool option by the same words.
int settingBoolWord(const std::string& text);

// The text of an int, -?[0-9]+ with leading zeros, into *out; false for any other text. The limits are not checked
// here. A number far outside what an option's limits can be is read as the edge it is beyond, so that it fails the
// range check and nothing overflows.
bool settingIntText(const std::string& text, int64_t* out);

// How an option stands in the file.
enum SettingState { kSettingUnset, kSettingSet, kSettingByHand };
const char* settingStateName(SettingState state);   // "unset", "set", "by-hand"

// The value text of the option's line, read. kSettingSet with *json the value as JSON text, or kSettingByHand with
// *raw what settingRaw() makes of the text.
SettingState readSettingValue(const ManifestSetting& option, const std::string& text, std::string* json, std::string* raw);

// A value the type does not take, as settings.get gives it: cut to kSettingsMaxRawBytes, and every byte that is not
// part of well-formed UTF-8 replaced by U+FFFD, so that it can travel as a JSON string.
std::string settingRaw(const std::string& text);

// A value a controller sent for the option. 0 with *line the text to write after KEY=, or the reason it is refused.
const char* checkSettingValue(const ManifestSetting& option, const json::Value& value, std::string* line);

// The option as settings.get gives it, as JSON: what the manifest says of it (key, label, description, type, the
// type's limits, default or unset, applies, advanced), then `state` as the file has it, with `value` or `raw`.
std::string settingJson(const ManifestSetting& option, const SettingsFile& file);

// Where the settings file of the package with this manifest is on this machine.
std::string settingsPath(const Manifest& m, const Platform& platform);

// The text of the package's settings file. *exists is false, and *text empty, where there is no file. False with
// *error saying why (for the log) where it cannot be read: see the top of this file.
bool readSettingsFile(const Manifest& m, const Platform& platform, std::string* text, bool* exists, std::string* error);

// Puts `text` in place of the package's settings file, or makes the file. False with *error saying why where
// something on the way is not as the top of this file says, the text is larger than kMaxSettingsFileBytes, or the
// file cannot be written.
bool writeSettingsFile(const Manifest& m, const Platform& platform, const std::string& text, std::string* error);

}  // namespace nbt

#endif

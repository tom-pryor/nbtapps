// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_FSUTIL_HPP
#define NBT_AGENT_FSUTIL_HPP

// File-system steps for the state store and the install engine, on plain POSIX calls. Two rules run through it:
//
//   - A file appears under its name only when it is complete. It is written as `.nbt-tmp` in the same folder,
//     synced, and renamed into place. One temporary name per folder is enough, because the agent does one thing at
//     a time, and a leftover is recognised and removed.
//   - Nothing here follows a link it is asked to remove, move or inspect: kindOf, removeFile and removeTree look at
//     the link itself.
//
// Each function that can fail returns false and sets *error to a line for the log; `error` may be 0.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "bytes.hpp"

namespace nbt {
namespace fs {

extern const char kTempName[];   // ".nbt-tmp"

enum Kind { kNone, kFile, kDir, kLink, kOther };
Kind kindOf(const std::string& path);   // of the path itself: a link is kLink whatever it points at

bool isDir(const std::string& path);    // a folder, or a link that leads to one

std::string parentOf(const std::string& path);   // "/a/b" -> "/a", "/a" -> "/", "a" -> "."
std::string tempIn(const std::string& dir);      // dir + "/.nbt-tmp"

// mkdir -p, mode 0755. True if the folder is there afterwards.
bool makeDirs(const std::string& path, std::string* error);
// One folder, whose parent exists. True if it was made; false with an empty *error if it was there already.
bool makeDir(const std::string& path, std::string* error);

// The whole file, if it is at most maxBytes.
bool readFile(const std::string& path, size_t maxBytes, std::string* out, std::string* error);
// Writes a file whole and durably: temporary, sync, rename, sync of the folder.
bool writeFile(const std::string& path, const std::string& data, unsigned mode, std::string* error);
// Appends to a file, creating it; for logs. Not synced.
bool appendFile(const std::string& path, const std::string& data, std::string* error);

// Copies a regular file the same way. `mode` < 0 keeps the mode and, where allowed, the owner of the source.
bool copyFile(const std::string& from, const std::string& to, int mode, std::string* error);
// Moves a regular file, replacing `to`: a rename, or across file systems a copy and then the removal of `from`.
// If it is interrupted, `to` is either absent or complete, and `from` is removed only after that.
bool moveFile(const std::string& from, const std::string& to, std::string* error);

// Removes a file or a link; true also when nothing is there.
bool removeFile(const std::string& path, std::string* error);
// Removes a folder if it is empty; true if it is gone afterwards. Leaves a folder that holds something.
bool removeEmptyDir(const std::string& path);
// rm -rf; true also when nothing is there.
bool removeTree(const std::string& path, std::string* error);
// Removes what a folder holds and leaves the folder; makes it if it is not there. A folder that is there and
// empty is not written to: the agent calls this at every start, on flash.
bool emptyDir(const std::string& path, std::string* error);

// The names in a folder, sorted, without "." and "..".
bool listDir(const std::string& path, std::vector<std::string>* names, std::string* error);

bool hashFile(const std::string& path, uint8_t out[32], uint64_t* size, std::string* error);
bool readLink(const std::string& path, std::string* target);
bool makeLink(const std::string& target, const std::string& path, std::string* error);

// Syncs a folder so that a rename or a removal in it survives a power cut. Best effort: some file systems refuse.
void syncDir(const std::string& dir);
// Syncs an open file. True if it is on disk (or syncing is switched off).
bool syncFd(int fd);
// Test only: `hook` is called between the parts of the steps above that are not one system call: after a file's
// bytes are written and before it is renamed into place, after that rename, and between the copy and the removal
// of a move across file systems. A test ends the process there to stand in for a crash.
typedef void (*TestHook)(const char* point);
void setTestHook(TestHook hook);
// Test only: with true, moveFile always copies and removes, as it must between two file systems. On the head unit
// the system partition and the agent's state are two file systems; in a test folder they are one.
void setCopyOnMove(bool on);
// Test only: with false, nothing is synced. A test that ends a process to stand in for a crash loses nothing by
// that, and thousands of syncs take minutes.
void setSyncing(bool on);

// Free bytes on the file system that holds `path`, or its nearest folder that exists.
bool freeBytes(const std::string& path, uint64_t* out);
// Whether the file system that holds `path`, or its nearest folder that exists, is mounted read-only.
bool readOnly(const std::string& path, bool* out);
// True if both paths, or their nearest folders that exist, are on one file system.
bool sameDevice(const std::string& a, const std::string& b);

// A file written in pieces and put in place at the end. Until commit() nothing appears under the name, and a
// writer that is dropped removes its temporary file.
class FileWriter : public ByteSink {
public:
    FileWriter() : fd_(-1) {}
    virtual ~FileWriter() { abandon(); }

    bool open(const std::string& path, std::string* error);
    virtual bool write(const void* data, size_t len);
    // `durable` syncs the file and its folder first; staging, which is emptied after a power cut, does without.
    bool commit(unsigned mode, bool durable, std::string* error);
    void abandon();

private:
    FileWriter(const FileWriter&);
    FileWriter& operator=(const FileWriter&);
    int fd_;
    std::string path_, temp_;
};

}  // namespace fs
}  // namespace nbt

#endif

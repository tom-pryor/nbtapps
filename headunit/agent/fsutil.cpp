// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// File-system steps: see fsutil.hpp.

#include "fsutil.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>

#include "sha256.hpp"

namespace nbt {
namespace fs {

const char kTempName[] = ".nbt-tmp";

namespace {

bool gSyncing = true;
bool gCopyOnMove = false;
TestHook gTestHook = 0;

void point(const char* name) {
    if (gTestHook != 0) gTestHook(name);
}

bool fail(std::string* error, const char* what, const std::string& path) {
    if (error != 0) *error = std::string(what) + " " + path + ": " + strerror(errno);
    return false;
}

bool writeAll(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        ssize_t n = ::write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// The nearest folder at or above `path` that exists.
std::string existingAncestor(std::string path) {
    struct stat st;
    while (stat(path.c_str(), &st) != 0) {
        std::string up = parentOf(path);
        if (up == path) break;
        path = up;
    }
    return path;
}

}  // namespace

Kind kindOf(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return kNone;
    if (S_ISREG(st.st_mode)) return kFile;
    if (S_ISDIR(st.st_mode)) return kDir;
    if (S_ISLNK(st.st_mode)) return kLink;
    return kOther;
}

bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string parentOf(const std::string& path) {
    size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    size_t slash = path.rfind('/', end == 0 ? 0 : end - 1);
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

std::string tempIn(const std::string& dir) { return dir + "/" + kTempName; }

bool makeDir(const std::string& path, std::string* error) {
    if (error != 0) error->clear();
    if (mkdir(path.c_str(), 0755) == 0) return true;
    if (errno == EEXIST && kindOf(path) == kDir) return false;
    return fail(error, "cannot make the folder", path);
}

bool makeDirs(const std::string& path, std::string* error) {
    Kind kind = kindOf(path);
    if (kind == kDir) return true;
    // A link to a folder counts: stat follows it.
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return true;
    std::string up = parentOf(path);
    if (up != path && !makeDirs(up, error)) return false;
    if (mkdir(path.c_str(), 0755) != 0 && !(errno == EEXIST && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))) {
        return fail(error, "cannot make the folder", path);
    }
    return true;
}

bool readFile(const std::string& path, size_t maxBytes, std::string* out, std::string* error) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return fail(error, "cannot open", path);
    out->clear();
    char buffer[16384];
    for (;;) {
        ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            fail(error, "cannot read", path);
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        if (out->size() + (size_t)n > maxBytes) {
            if (error != 0) *error = path + " is larger than expected";
            ::close(fd);
            return false;
        }
        out->append(buffer, (size_t)n);
    }
    ::close(fd);
    return true;
}

bool writeFile(const std::string& path, const std::string& data, unsigned mode, std::string* error) {
    FileWriter writer;
    if (!writer.open(path, error)) return false;
    if (!data.empty() && !writer.write(data.data(), data.size())) return fail(error, "cannot write", path);
    return writer.commit(mode, true, error);
}

bool appendFile(const std::string& path, const std::string& data, std::string* error) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return fail(error, "cannot open", path);
    bool ok = writeAll(fd, data.data(), data.size());
    if (!ok) fail(error, "cannot write", path);
    ::close(fd);
    return ok;
}

bool copyFile(const std::string& from, const std::string& to, int mode, std::string* error) {
    int in = ::open(from.c_str(), O_RDONLY);
    struct stat st;
    if (in < 0 || fstat(in, &st) != 0) {
        fail(error, "cannot open", from);
        if (in >= 0) ::close(in);
        return false;
    }
    FileWriter writer;
    if (!writer.open(to, error)) {
        ::close(in);
        return false;
    }
    char buffer[32768];
    for (;;) {
        ssize_t n = ::read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            fail(error, "cannot read", from);
            ::close(in);
            return false;
        }
        if (n == 0) break;
        if (!writer.write(buffer, (size_t)n)) {
            fail(error, "cannot write", to);
            ::close(in);
            return false;
        }
    }
    ::close(in);
    if (!writer.commit(mode < 0 ? (unsigned)(st.st_mode & 07777) : (unsigned)mode, true, error)) return false;
    // The owner goes with a file that keeps its mode: a stock file put back should be as it was. Not being allowed
    // to (the host tests do not run as root) is not a failure.
    if (mode < 0 && chown(to.c_str(), st.st_uid, st.st_gid) != 0) errno = 0;
    return true;
}

bool moveFile(const std::string& from, const std::string& to, std::string* error) {
    if (!gCopyOnMove) {
        if (rename(from.c_str(), to.c_str()) == 0) {
            syncDir(parentOf(to));
            syncDir(parentOf(from));
            return true;
        }
        if (errno != EXDEV) return fail(error, "cannot move", from);
    } else if (kindOf(from) != kFile) {
        errno = ENOENT;
        return fail(error, "cannot move", from);
    }
    if (!copyFile(from, to, -1, error)) return false;
    point("copied");
    if (!removeFile(from, error)) return false;
    syncDir(parentOf(from));
    return true;
}

bool removeFile(const std::string& path, std::string* error) {
    if (unlink(path.c_str()) == 0 || errno == ENOENT || errno == ENOTDIR) return true;
    return fail(error, "cannot remove", path);
}

bool removeEmptyDir(const std::string& path) { return rmdir(path.c_str()) == 0 || errno == ENOENT; }

bool removeTree(const std::string& path, std::string* error) {
    Kind kind = kindOf(path);
    if (kind == kNone) return true;
    if (kind != kDir) return removeFile(path, error);
    std::vector<std::string> names;
    if (!listDir(path, &names, error)) return false;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!removeTree(path + "/" + names[i], error)) return false;
    }
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) return fail(error, "cannot remove the folder", path);
    return true;
}

bool emptyDir(const std::string& path, std::string* error) {
    if (kindOf(path) != kDir) return removeTree(path, error) && makeDirs(path, error);
    std::vector<std::string> names;
    if (!listDir(path, &names, error)) return false;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!removeTree(path + "/" + names[i], error)) return false;
    }
    return true;
}

bool listDir(const std::string& path, std::vector<std::string>* names, std::string* error) {
    names->clear();
    DIR* dir = opendir(path.c_str());
    if (dir == 0) return fail(error, "cannot list", path);
    for (;;) {
        struct dirent* entry = readdir(dir);
        if (entry == 0) break;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        names->push_back(entry->d_name);
    }
    closedir(dir);
    std::sort(names->begin(), names->end());
    return true;
}

bool hashFile(const std::string& path, uint8_t out[32], uint64_t* size, std::string* error) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return fail(error, "cannot open", path);
    Sha256 hash;
    uint64_t total = 0;
    char buffer[32768];
    for (;;) {
        ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            fail(error, "cannot read", path);
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        hash.update(buffer, (size_t)n);
        total += (uint64_t)n;
    }
    ::close(fd);
    hash.finish(out);
    if (size != 0) *size = total;
    return true;
}

bool readLink(const std::string& path, std::string* target) {
    char buffer[1024];
    ssize_t n = readlink(path.c_str(), buffer, sizeof buffer);
    if (n < 0 || (size_t)n >= sizeof buffer) return false;
    target->assign(buffer, (size_t)n);
    return true;
}

bool makeLink(const std::string& target, const std::string& path, std::string* error) {
    if (symlink(target.c_str(), path.c_str()) != 0) return fail(error, "cannot make the link", path);
    syncDir(parentOf(path));
    return true;
}

void setSyncing(bool on) { gSyncing = on; }
void setTestHook(TestHook hook) { gTestHook = hook; }
void setCopyOnMove(bool on) { gCopyOnMove = on; }

bool syncFd(int fd) { return !gSyncing || fsync(fd) == 0; }

void syncDir(const std::string& dir) {
    if (!gSyncing) return;
    int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) return;
    if (fsync(fd) != 0) errno = 0;
    ::close(fd);
}

bool freeBytes(const std::string& path, uint64_t* out) {
    struct statvfs vfs;
    if (statvfs(existingAncestor(path).c_str(), &vfs) != 0) return false;
    *out = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    return true;
}

bool readOnly(const std::string& path, bool* out) {
    struct statvfs vfs;
    if (statvfs(existingAncestor(path).c_str(), &vfs) != 0) return false;
    *out = (vfs.f_flag & ST_RDONLY) != 0;
    return true;
}

bool sameDevice(const std::string& a, const std::string& b) {
    struct stat sa, sb;
    if (stat(existingAncestor(a).c_str(), &sa) != 0 || stat(existingAncestor(b).c_str(), &sb) != 0) return false;
    return sa.st_dev == sb.st_dev;
}

// ---- FileWriter ---------------------------------------------------------------------------------------------------

bool FileWriter::open(const std::string& path, std::string* error) {
    abandon();
    path_ = path;
    temp_ = tempIn(parentOf(path));
    // A leftover from an interrupted write is replaced. O_EXCL also refuses to write through a link left there.
    if (unlink(temp_.c_str()) != 0 && errno != ENOENT) return fail(error, "cannot remove", temp_);
    fd_ = ::open(temp_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd_ < 0) return fail(error, "cannot create a file in", parentOf(path));
    return true;
}

bool FileWriter::write(const void* data, size_t len) { return fd_ >= 0 && writeAll(fd_, data, len); }

bool FileWriter::commit(unsigned mode, bool durable, std::string* error) {
    if (fd_ < 0) {
        if (error != 0) *error = "nothing is open to put in place";
        return false;
    }
    bool ok = fchmod(fd_, (mode_t)mode) == 0 && (!durable || syncFd(fd_));
    if (!ok) fail(error, "cannot finish writing", path_);
    if (::close(fd_) != 0 && ok) ok = fail(error, "cannot finish writing", path_);
    fd_ = -1;
    if (ok) point("written");
    if (ok && rename(temp_.c_str(), path_.c_str()) != 0) ok = fail(error, "cannot put in place", path_);
    if (!ok) {
        unlink(temp_.c_str());
        return false;
    }
    if (durable) syncDir(parentOf(path_));
    point("renamed");
    return true;
}

void FileWriter::abandon() {
    if (fd_ < 0) return;
    ::close(fd_);
    fd_ = -1;
    unlink(temp_.c_str());
}

}  // namespace fs
}  // namespace nbt

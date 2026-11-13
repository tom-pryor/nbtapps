// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The package reader: see package.hpp. The zip structures are read by hand; only inflate and CRC32 come from zlib.

#include "package.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <set>

#include "ed25519.hpp"
#include "hex.hpp"
#include "inflate.hpp"
#include "sha256.hpp"
#include "utf8.hpp"

namespace nbt {

namespace {

const char kManifestName[] = "manifest.json";
const char kSignatureName[] = "manifest.sig";

const uint32_t kLocalSig = 0x04034B50, kCentralSig = 0x02014B50, kEndSig = 0x06054B50;
const size_t kLocalSize = 30, kCentralSize = 46, kEndSize = 22;
const uint16_t kStored = 0, kDeflated = 8;
const uint16_t kFlagEncrypted = 0x2041;   // bit 0 encrypted, bit 6 strong encryption, bit 13 masked directory
const uint16_t kFlagDescriptor = 0x0008;

uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

bool badArchive(Error* error, const std::string& message) { return error->fail("bad-archive", message); }
bool unreadable(Error* error) { return error->fail("io-error", "the archive could not be read"); }

}  // namespace

// ---- sources ------------------------------------------------------------------------------------------------------

bool MemorySource::read(uint64_t offset, void* out, size_t len) {
    if (offset > len_ || len > len_ - offset) return false;
    if (len > 0) memcpy(out, data_ + offset, len);
    return true;
}

bool FileSource::open(const std::string& path, std::string* error) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY);
    struct stat st;
    if (fd_ < 0 || fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (error != 0) *error = "cannot open " + path + ": " + (fd_ < 0 ? strerror(errno) : "not a regular file");
        close();
        return false;
    }
    size_ = (uint64_t)st.st_size;
    return true;
}

void FileSource::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    size_ = 0;
}

bool FileSource::read(uint64_t offset, void* out, size_t len) {
    uint8_t* p = (uint8_t*)out;
    while (len > 0) {
        ssize_t n = pread(fd_, p, len, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;   // an error, or the file is shorter than it was
        p += n;
        offset += (uint64_t)n;
        len -= (size_t)n;
    }
    return true;
}

// ---- reading in order ---------------------------------------------------------------------------------------------

// Reads a stretch of the source from front to back through one buffer. With a hash given, every byte that passes
// goes into it: that is how open() gets the archive's own SHA-256 out of the pass that checks the entries.
class PackageCursor {
public:
    PackageCursor(ByteSource* source, uint64_t begin, uint64_t end, Sha256* hash)
        : source_(source), at_(begin), end_(end), hash_(hash), buffer_(32768), have_(0), used_(0) {}

    uint64_t at() const { return at_; }

    // The next bytes, at most `max` of them: at least one unless the stretch has ended. False if the source fails.
    bool next(const uint8_t** data, size_t* len, uint64_t max) {
        if (used_ == have_) {
            uint64_t left = end_ - at_;
            have_ = left < buffer_.size() ? (size_t)left : buffer_.size();
            used_ = 0;
            if (have_ > 0 && !source_->read(at_, &buffer_[0], have_)) {
                have_ = 0;
                return false;
            }
        }
        size_t n = have_ - used_;
        if (n > max) n = (size_t)max;
        *data = &buffer_[0] + used_;
        *len = n;
        if (hash_ != 0 && n > 0) hash_->update(*data, n);
        used_ += n;
        at_ += n;
        return true;
    }

    // Exactly `len` bytes; the caller has checked that the stretch holds them.
    bool take(void* out, size_t len) {
        uint8_t* p = (uint8_t*)out;
        while (len > 0) {
            const uint8_t* data;
            size_t n;
            if (!next(&data, &n, len) || n == 0) return false;
            memcpy(p, data, n);
            p += n;
            len -= n;
        }
        return true;
    }

    bool skip(uint64_t len) {
        while (len > 0) {
            const uint8_t* data;
            size_t n;
            if (!next(&data, &n, len) || n == 0) return false;
            len -= n;
        }
        return true;
    }

private:
    ByteSource* source_;
    uint64_t at_, end_;
    Sha256* hash_;
    std::vector<uint8_t> buffer_;
    size_t have_, used_;
};

// ---- the container ------------------------------------------------------------------------------------------------

bool Package::readDirectory(Error* error) {
    const char* const damaged = "the archive's file list is damaged";
    uint64_t dirEnd = dirOffset_ + dirSize_;
    PackageCursor cursor(source_, dirOffset_, dirEnd, 0);
    std::set<std::string> names;
    entries_.reserve(count_);
    for (unsigned i = 0; i < count_; ++i) {
        if (cursor.at() + kCentralSize > dirEnd) return badArchive(error, damaged);
        uint8_t h[kCentralSize];
        if (!cursor.take(h, sizeof h)) return unreadable(error);
        Entry e;
        memset(e.sha256, 0, sizeof e.sha256);
        e.flags = u16(h + 8);
        e.method = u16(h + 10);
        e.crc = u32(h + 16);
        e.csize = u32(h + 20);
        e.usize = u32(h + 24);
        size_t nameLen = u16(h + 28), extraLen = u16(h + 30), noteLen = u16(h + 32);
        unsigned startDisk = u16(h + 34);
        e.offset = u32(h + 42);
        e.dataOffset = 0;
        if (u32(h) != kCentralSig || cursor.at() + nameLen + extraLen + noteLen > dirEnd) return badArchive(error, damaged);
        if (nameLen > kMaxPathBytes) return badArchive(error, "a path in the archive is longer than 255 bytes");
        e.name.resize(nameLen);
        if (nameLen > 0 && !cursor.take(&e.name[0], nameLen)) return unreadable(error);
        if (!cursor.skip(extraLen + noteLen)) return unreadable(error);

        if (!validUtf8(e.name)) return badArchive(error, "a file name is not valid UTF-8");
        e.dir = !e.name.empty() && e.name[e.name.size() - 1] == '/';
        if (!validRelativePath(e.name, e.dir)) return badArchive(error, "a path is not allowed: " + e.name);
        if (!names.insert(e.name).second) return badArchive(error, e.name + " is in the archive twice");
        if (e.flags & kFlagEncrypted) return badArchive(error, e.name + " is encrypted");
        if (e.flags & kFlagDescriptor) return badArchive(error, e.name + " uses a data descriptor");
        if (e.method != kStored && e.method != kDeflated) return badArchive(error, e.name + " uses a compression that is not allowed");
        if (e.csize == 0xFFFFFFFFu || e.usize == 0xFFFFFFFFu || e.offset == 0xFFFFFFFFu) return badArchive(error, "the archive uses zip64");
        if (startDisk != 0) return badArchive(error, "the archive layout is not allowed");
        if (e.dir && (e.method != kStored || e.csize != 0 || e.usize != 0)) return badArchive(error, "the folder entry " + e.name + " holds data");
        entries_.push_back(e);
    }
    if (cursor.at() != dirEnd) return badArchive(error, damaged);
    return true;
}

bool Package::readData(const Entry& entry, PackageCursor* cursor, ByteSink* sink, std::string* keep, uint8_t hashOut[32],
                       Error* error) {
    const std::string damaged = entry.name + " is damaged";
    Sha256 hash;
    uint32_t crc = 0;
    uint64_t produced = 0;
    uint64_t remaining = entry.csize;

    if (entry.method == kStored) {
        if (entry.csize != entry.usize) return badArchive(error, damaged);
        while (remaining > 0) {
            const uint8_t* data;
            size_t n;
            if (!cursor->next(&data, &n, remaining) || n == 0) return unreadable(error);
            remaining -= n;
            produced += n;
            hash.update(data, n);
            crc = crc32(crc, data, n);
            if (keep != 0) keep->append((const char*)data, n);
            if (sink != 0 && !sink->write(data, n)) return error->fail("io-error", "could not write " + entry.name);
        }
    } else {
        Inflater inflater;
        std::vector<uint8_t> out(32768);
        bool done = false;
        while (!done) {
            // The stream must end exactly where the entry does: not before its last byte and not after it.
            if (remaining == 0) return badArchive(error, damaged);
            const uint8_t* in;
            size_t inLen;
            if (!cursor->next(&in, &inLen, remaining) || inLen == 0) return unreadable(error);
            remaining -= inLen;
            for (;;) {
                size_t got = 0;
                Inflater::Result result = inflater.inflate(&in, &inLen, &out[0], out.size(), &got);
                produced += got;
                if (produced > entry.usize) return badArchive(error, damaged);
                if (got > 0) {
                    hash.update(&out[0], got);
                    crc = crc32(crc, &out[0], got);
                    if (keep != 0) keep->append((const char*)&out[0], got);
                    if (sink != 0 && !sink->write(&out[0], got)) return error->fail("io-error", "could not write " + entry.name);
                }
                if (result == Inflater::kError) return badArchive(error, damaged);
                if (result == Inflater::kDone) {
                    if (inLen != 0 || remaining != 0) return badArchive(error, damaged);
                    done = true;
                    break;
                }
                if (result == Inflater::kNeedInput) break;
            }
        }
    }
    if (produced != entry.usize || crc != entry.crc) return badArchive(error, damaged);
    hash.finish(hashOut);
    return true;
}

bool Package::readEntries(Error* error) {
    Sha256 archive;
    PackageCursor cursor(source_, 0, dirOffset_, &archive);
    uint64_t expected = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        const std::string damaged = e.name + " is damaged";
        // Entries sit one after another in directory order, from byte 0 up to the directory. A reader that walks
        // the local headers of a stream and a reader that starts from the directory then see the same files.
        if (e.offset != expected) return badArchive(error, e.name + " is not where the entry before it ends");
        if (expected + kLocalSize > dirOffset_) return badArchive(error, damaged);
        uint8_t h[kLocalSize];
        if (!cursor.take(h, sizeof h)) return unreadable(error);
        size_t nameLen = u16(h + 26), extraLen = u16(h + 28);
        uint64_t start = expected + kLocalSize + nameLen + extraLen;
        if (u32(h) != kLocalSig || start + e.csize > dirOffset_) return badArchive(error, damaged);
        std::string localName(nameLen, '\0');
        if (nameLen > 0 && !cursor.take(&localName[0], nameLen)) return unreadable(error);
        if (u16(h + 6) != e.flags || u16(h + 8) != e.method || u32(h + 14) != e.crc || u32(h + 18) != e.csize ||
            u32(h + 22) != e.usize || localName != e.name) {
            return badArchive(error, e.name + " is described in two different ways");
        }
        if (!cursor.skip(extraLen)) return unreadable(error);
        e.dataOffset = start;

        // The manifest and its signature are the only entries kept in memory, and only at sizes that can be right.
        std::string* keep = 0;
        if (i == 0 && e.name == kManifestName && e.usize <= kMaxManifestBytes) keep = &manifestBytes_;
        if (i == 1 && e.name == kSignatureName && e.usize == kEd25519SignatureBytes) keep = &signatureBytes_;
        if (!readData(e, &cursor, 0, keep, e.sha256, error)) return false;
        expected = start + e.csize;
    }
    if (expected != dirOffset_) return badArchive(error, "the archive holds bytes that belong to no file");

    // The rest of the archive's own hash: the directory and the end record.
    PackageCursor tail(source_, dirOffset_, size_, &archive);
    if (!tail.skip(size_ - dirOffset_)) return unreadable(error);
    uint8_t digest[kSha256Bytes];
    archive.finish(digest);
    sha256_ = toHex(digest, sizeof digest);
    return true;
}

bool Package::open(ByteSource* source, const std::vector<TrustedKey>& keys, Error* error) {
    return open(source, keys, std::string(), error);
}

bool Package::open(ByteSource* source, const std::vector<TrustedKey>& keys, const std::string& signature, Error* error) {
    source_ = source;
    size_ = source->size();
    entries_.clear();
    files_.clear();
    manifestBytes_.clear();
    signatureBytes_.clear();
    keyIndex_ = -1;

    // 1. The container. With no archive comment the end record is exactly the last 22 bytes.
    if (size_ < kEndSize) return badArchive(error, "the file is too short to be a package");
    uint8_t end[kEndSize];
    if (!source_->read(size_ - kEndSize, end, sizeof end)) return unreadable(error);
    if (u32(end) != kEndSig) {
        return badArchive(error, "the file is not a package, is cut short, or has an archive comment or bytes after its end");
    }
    unsigned disk = u16(end + 4), dirDisk = u16(end + 6), onDisk = u16(end + 8), comment = u16(end + 20);
    count_ = u16(end + 10);
    dirSize_ = u32(end + 12);
    dirOffset_ = u32(end + 16);
    if (count_ == 0xFFFF || dirSize_ == 0xFFFFFFFFu || dirOffset_ == 0xFFFFFFFFu) return badArchive(error, "the archive uses zip64");
    if (disk != 0 || dirDisk != 0 || onDisk != count_ || comment != 0) return badArchive(error, "the archive layout is not allowed");
    if (dirOffset_ + dirSize_ != size_ - kEndSize) return badArchive(error, "the archive's file list is damaged");
    if (!readDirectory(error) || !readEntries(error)) return false;

    // 2. The manifest is the first entry.
    if (entries_.empty() || entries_[0].name != kManifestName) {
        return badArchive(error, "the package does not start with its manifest");
    }
    if (entries_[0].usize > kMaxManifestBytes) return error->fail("bad-manifest", "manifest is larger than 1 MiB");

    // 3. The signature is the second, and is checked before the manifest is parsed. A detached one is checked
    //    instead, and manifest.sig is then not read.
    bool hasSig = entries_.size() >= 2 && entries_[1].name == kSignatureName;
    if (!hasSig) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].name == kSignatureName) return badArchive(error, "manifest.sig is not the second entry");
        }
    }
    if (!signature.empty()) {
        if (signature.size() != kEd25519SignatureBytes) {
            return error->fail("bad-signature", "the signature given is not 64 bytes");
        }
        signatureBytes_ = signature;
    } else if (!hasSig) {
        return error->fail("bad-signature", "the package is not signed");
    } else if (entries_[1].usize != kEd25519SignatureBytes) {
        return error->fail("bad-signature", "manifest.sig is not 64 bytes");
    }
    for (size_t i = 0; i < keys.size() && keyIndex_ < 0; ++i) {
        if (ed25519Verify(keys[i].key, manifestBytes_.data(), manifestBytes_.size(), (const uint8_t*)signatureBytes_.data())) {
            keyIndex_ = (int)i;
        }
    }
    if (keyIndex_ < 0) return error->fail("bad-signature", "no trusted key verifies the signature");

    // 4. The manifest.
    if (!parseManifest(manifestBytes_.data(), manifestBytes_.size(), &manifest_, error)) return false;
    manifestSha256_ = toHex(entries_[0].sha256, kSha256Bytes);

    // 5. The files are exactly the ones the manifest lists.
    for (size_t i = hasSig ? 2 : 1; i < entries_.size(); ++i) {
        if (!entries_[i].dir) files_[entries_[i].name] = i;
    }
    for (size_t i = 0; i < manifest_.contents.size(); ++i) {
        const ManifestContent& content = manifest_.contents[i];
        std::map<std::string, size_t>::const_iterator it = files_.find(content.path);
        if (it == files_.end()) return badArchive(error, content.path + " is listed in the manifest but is not in the package");
        const Entry& e = entries_[it->second];
        if (e.usize != content.size || toHex(e.sha256, kSha256Bytes) != content.sha256) {
            return badArchive(error, content.path + " is not the file the manifest describes");
        }
    }
    if (files_.size() != manifest_.contents.size()) {
        // Paths in contents are unique and each was found, so a file is left over. Name the first one.
        std::set<std::string> listed;
        for (size_t i = 0; i < manifest_.contents.size(); ++i) listed.insert(manifest_.contents[i].path);
        for (std::map<std::string, size_t>::const_iterator it = files_.begin(); it != files_.end(); ++it) {
            if (listed.count(it->first) == 0) return badArchive(error, it->first + " is in the package but not in its manifest");
        }
    }
    return true;
}

bool Package::extract(const std::string& path, ByteSink* sink, Error* error) {
    std::map<std::string, size_t>::const_iterator it = files_.find(path);
    if (it == files_.end()) return badArchive(error, path + " is not in the package");
    const Entry& e = entries_[it->second];
    PackageCursor cursor(source_, e.dataOffset, e.dataOffset + e.csize, 0);
    uint8_t digest[kSha256Bytes];
    if (!readData(e, &cursor, sink, 0, digest, error)) return false;
    if (memcmp(digest, e.sha256, sizeof digest) != 0) return badArchive(error, path + " changed while it was being installed");
    return true;
}

}  // namespace nbt

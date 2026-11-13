// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_PACKAGE_HPP
#define NBT_AGENT_PACKAGE_HPP

// Reader for a .nbtpkg: the zip profile, the signature, the manifest and every file's hash. It makes the checks
// tools/nbtpkg.py makes (`verify`), in the same order, so the two give the same code for the same file:
//
//   1. the container: end record, directory, each local header equal to its directory record, entries one after
//      another from byte 0, every entry inflated and its CRC checked                       bad-archive
//   2. manifest.json is the first entry                                                   bad-archive
//      and at most 1 MiB                                                                  bad-manifest
//   3. manifest.sig is the second entry (elsewhere: bad-archive), is 64 bytes, and one of the trusted keys
//      verifies it over the exact bytes of manifest.json                                  bad-signature
//      With a detached signature given to open(), as a catalogue gives one for a package whose author signed it
//      with a key of their own, that signature is the one checked. manifest.sig is then not read: it may be
//      missing or signed by any key, and only has to be the second entry if it is there.
//   4. the manifest itself, see manifest.hpp                               bad-manifest, unsupported-manifest
//   5. the files are exactly the ones `contents` lists, with those sizes and hashes        bad-archive
//
// The signature is checked before the manifest is parsed, so only authenticated bytes reach the JSON parser.
// The archive is read in pieces and never held whole: memory use is the directory (at most 65,534 names of at most
// 255 bytes) plus the manifest. open() reads every byte once; extract() reads one entry again and checks it again.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "bytes.hpp"
#include "error.hpp"
#include "manifest.hpp"

namespace nbt {

class PackageCursor;

class MemorySource : public ByteSource {
public:
    MemorySource(const void* data, size_t len) : data_((const uint8_t*)data), len_(len) {}
    virtual uint64_t size() const { return len_; }
    virtual bool read(uint64_t offset, void* out, size_t len);

private:
    const uint8_t* data_;
    size_t len_;
};

class FileSource : public ByteSource {
public:
    FileSource() : fd_(-1), size_(0) {}
    virtual ~FileSource() { close(); }
    bool open(const std::string& path, std::string* error);
    void close();
    virtual uint64_t size() const { return size_; }
    virtual bool read(uint64_t offset, void* out, size_t len);

private:
    FileSource(const FileSource&);
    FileSource& operator=(const FileSource&);
    int fd_;
    uint64_t size_;
};

struct TrustedKey {
    uint8_t key[32];     // an Ed25519 public key
    std::string name;    // the source it belongs to
    bool embedded;       // built into the agent, which is what the reserved org.nbtapp. ids need

    TrustedKey() : embedded(false) {
        for (int i = 0; i < 32; ++i) key[i] = 0;
    }
};

class Package {
public:
    Package() : source_(0), size_(0), dirOffset_(0), dirSize_(0), count_(0), keyIndex_(-1) {}

    // Checks the whole archive against `keys`. `source` must stay alive for as long as extract() is used.
    // On a refusal returns false with error->code set as listed above; an archive that cannot be read is io-error.
    bool open(ByteSource* source, const std::vector<TrustedKey>& keys, Error* error);
    // The same with a detached signature over the manifest (64 bytes), checked instead of manifest.sig. An empty
    // `signature` is the same as the call above.
    bool open(ByteSource* source, const std::vector<TrustedKey>& keys, const std::string& signature, Error* error);

    const Manifest& manifest() const { return manifest_; }
    const std::string& manifestBytes() const { return manifestBytes_; }     // as signed
    // The 64 bytes of the signature that was checked: manifest.sig's, or the detached one given to open().
    const std::string& signatureBytes() const { return signatureBytes_; }
    // What an index entry or a job.plan offer says about this archive, all as lowercase hex.
    const std::string& sha256() const { return sha256_; }
    uint64_t size() const { return size_; }
    const std::string& manifestSha256() const { return manifestSha256_; }
    // The trusted key that verified the signature: its position in `keys`.
    int keyIndex() const { return keyIndex_; }

    // Writes one file to `sink`. `path` is a path from the manifest's contents. The size, the CRC and the SHA-256
    // are checked again as the bytes pass; on a mismatch (bad-archive: the file changed since open()) or a failed
    // write (io-error) the caller discards what the sink received.
    bool extract(const std::string& path, ByteSink* sink, Error* error);

private:
    Package(const Package&);
    Package& operator=(const Package&);

    struct Entry {
        std::string name;
        bool dir;
        uint16_t flags, method;
        uint32_t crc, csize, usize;
        uint32_t offset;       // of the local header, from the directory
        uint64_t dataOffset;   // of the entry's bytes
        uint8_t sha256[32];    // of the uncompressed bytes
    };

    bool readDirectory(Error* error);
    bool readEntries(Error* error);
    // Reads one entry's bytes from the cursor, inflating as needed, and checks size and CRC. The uncompressed
    // bytes go to `sink` and to `keep` (either may be 0) and into the SHA-256 written to hashOut.
    static bool readData(const Entry& entry, PackageCursor* cursor, ByteSink* sink, std::string* keep,
                         uint8_t hashOut[32], Error* error);

    ByteSource* source_;
    uint64_t size_;
    uint64_t dirOffset_, dirSize_;
    unsigned count_;
    std::vector<Entry> entries_;
    std::map<std::string, size_t> files_;   // manifest path -> entry

    Manifest manifest_;
    std::string manifestBytes_, signatureBytes_;
    std::string sha256_, manifestSha256_;
    int keyIndex_;
};

}  // namespace nbt

#endif

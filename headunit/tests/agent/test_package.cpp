// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/package.cpp against the packages tests/make_packages.py builds: every good one opens with the
// values vectors.json gives it, every bad one is refused with its code, and a package cut short or changed in any
// one byte is never read as something else.
//
// usage: test_package [testdata folder]     (default build/testdata, as `make` runs it)
//        test_package --codes FILE...       prints "<code or ok> FILE" for each, for comparing readers

#include "../../agent/package.hpp"

#include <string.h>

#include "../../agent/json.hpp"
#include "../../agent/sha256.hpp"
#include "test_util.hpp"

using namespace nbt;
using testutil::readFile;

static std::string hashOf(const std::string& data) {
    uint8_t digest[kSha256Bytes];
    sha256(data.data(), data.size(), digest);
    return toHex(digest, sizeof digest);
}

// "" when the package opens, otherwise the code of the refusal.
static std::string codeOf(const std::string& data, const std::vector<TrustedKey>& keys) {
    MemorySource source(data.data(), data.size());
    Package package;
    Error error;
    bool ok = package.open(&source, keys, &error);
    CHECK(ok == error.code.empty() && (ok || !error.message.empty()));
    return error.code;
}

// A source that fails after a number of reads, for the io-error path.
class FailingSource : public ByteSource {
public:
    FailingSource(const std::string& data, int readsAllowed) : data_(data), left_(readsAllowed) {}
    virtual uint64_t size() const { return data_.size(); }
    virtual bool read(uint64_t offset, void* out, size_t len) {
        if (left_-- <= 0 || offset + len > data_.size()) return false;
        memcpy(out, data_.data() + offset, len);
        return true;
    }

private:
    std::string data_;
    int left_;
};

class RefusingSink : public ByteSink {
public:
    virtual bool write(const void*, size_t) { return false; }
};

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--codes") == 0) {
        // Trusts the test package key only, as tests/test_nbtpkg.py does for the bad packages.
        std::vector<TrustedKey> keys;
        keys.push_back(testutil::key("18680457e3b23112c2f64935bdf852c263df25880d99ece34561c01c76a9758d", "test", true));
        for (int i = 2; i < argc; ++i) {
            std::string code = codeOf(readFile(argv[i]), keys);
            printf("%s %s\n", code.empty() ? "ok" : code.c_str(), argv[i]);
        }
        return 0;
    }

    std::string dir = argc > 1 ? argv[1] : "build/testdata";
    std::string vectorsText = readFile(dir + "/vectors.json");
    json::Document vectors;
    std::string why;
    if (!vectors.parse(vectorsText, &why)) {
        fprintf(stderr, "test_package: vectors.json: %s\n", why.c_str());
        return 1;
    }
    json::Value root = vectors.root();
    std::string packageKeyHex = root.find("keys").find("package").string();
    std::string otherKeyHex = root.find("keys").find("other").string();
    std::vector<TrustedKey> packageOnly, otherOnly, both, none;
    packageOnly.push_back(testutil::key(packageKeyHex, "package", true));
    otherOnly.push_back(testutil::key(otherKeyHex, "other", false));
    both.push_back(otherOnly[0]);
    both.push_back(packageOnly[0]);

    // 1. Good packages: the values of an offer, the key that verified, and every file extracted.
    json::Value good = root.find("good");
    CHECK(good.size() >= 6);
    std::string hello;
    for (size_t i = 0; i < good.size(); ++i) {
        json::Value v = good.at(i);
        std::string file = v.find("file").string();
        std::string data = readFile(dir + "/" + file);

        FileSource source;
        CHECK_IN(file, source.open(dir + "/" + file, &why));
        Package package;
        Error error;
        CHECK_IN(file + ": " + error.message, package.open(&source, both, &error));
        CHECK_IN(file, package.keyIndex() == 1);
        CHECK_IN(file, package.manifest().id == v.find("id").string());
        CHECK_IN(file, package.manifest().version == v.find("version").string());
        CHECK_IN(file, package.manifest().name == v.find("name").string());
        CHECK_IN(file, package.sha256() == v.find("sha256").string() && package.sha256() == hashOf(data));
        CHECK_IN(file, package.size() == (uint64_t)v.find("size").integer() && package.size() == data.size());
        CHECK_IN(file, package.manifestSha256() == v.find("manifestSha256").string());
        CHECK_IN(file, package.manifestSha256() == hashOf(package.manifestBytes()));
        CHECK_IN(file, toHex((const uint8_t*)package.signatureBytes().data(), package.signatureBytes().size()) ==
                           v.find("signature").string());
        std::vector<std::string> flags = riskFlags(package.manifest());
        CHECK_IN(file, flags.size() == v.find("flags").size());
        for (size_t k = 0; k < flags.size(); ++k) CHECK_IN(file, flags[k] == v.find("flags").at(k).string());

        const Manifest& m = package.manifest();
        for (size_t k = 0; k < m.contents.size(); ++k) {
            testutil::StringSink sink;
            CHECK_IN(file + ": " + m.contents[k].path, package.extract(m.contents[k].path, &sink, &error));
            CHECK_IN(file, sink.text.size() == m.contents[k].size && hashOf(sink.text) == m.contents[k].sha256);
        }
        // Only what the manifest lists can be asked for.
        testutil::StringSink sink;
        Error missing;
        CHECK(!package.extract("manifest.json", &sink, &missing) && missing.code == "bad-archive");

        // The same bytes from memory, and with the wrong keys or none.
        CHECK_IN(file, codeOf(data, packageOnly) == "");
        CHECK_IN(file, codeOf(data, otherOnly) == "bad-signature" && codeOf(data, none) == "bad-signature");
        if (file == "good/org.nbtapp.hello-1.2.0.nbtpkg") hello = data;
    }
    CHECK(!hello.empty());

    // 2. Bad packages: each refused, with the code it was built to get.
    json::Value bad = root.find("bad");
    CHECK(bad.size() >= 50);
    for (size_t i = 0; i < bad.size(); ++i) {
        json::Value v = bad.at(i);
        std::string file = v.find("file").string();
        std::string code = codeOf(readFile(dir + "/" + file), packageOnly);
        CHECK_IN(file + " (" + v.find("what").string() + "): got " + (code.empty() ? "accepted" : code),
                 code == v.find("code").string());
    }

    // 3. A file extracted is the file in the source tree, and a sink that refuses is an io-error.
    {
        MemorySource source(hello.data(), hello.size());
        Package package;
        Error error;
        CHECK(package.open(&source, packageOnly, &error));
        testutil::StringSink sink;
        CHECK(package.extract("payload/bin/hello", &sink, &error));
        CHECK(sink.text == readFile("tests/packages/hello-1.2.0/payload/bin/hello"));
        RefusingSink refusing;
        CHECK(!package.extract("payload/bin/hello", &refusing, &error) && error.code == "io-error");
    }

    // 4. Cut short at every length: refused as a broken archive, never read as a smaller package.
    for (size_t n = 0; n < hello.size(); ++n) CHECK_IN("cut", codeOf(hello.substr(0, n), packageOnly) == "bad-archive");

    // 5. One byte changed, at every position. Most changes are refused. The ones that are not sit in fields the
    //    profile does not use (timestamps, versions, attributes), and the package is then read exactly as before.
    size_t accepted = 0;
    for (size_t at = 0; at < hello.size(); ++at) {
        std::string changed = hello;
        changed[at] = (char)(changed[at] ^ 0x41);
        MemorySource source(changed.data(), changed.size());
        Package package;
        Error error;
        if (!package.open(&source, packageOnly, &error)) continue;
        ++accepted;
        CHECK(package.manifestSha256() == hashOf(package.manifestBytes()));
        const Manifest& m = package.manifest();
        for (size_t k = 0; k < m.contents.size(); ++k) {
            testutil::StringSink sink;
            CHECK(package.extract(m.contents[k].path, &sink, &error) && hashOf(sink.text) == m.contents[k].sha256);
        }
    }
    CHECK(accepted > 0 && accepted < hello.size() / 4);

    // 6. A source that stops answering: an io-error at whatever read it happens, never a wrong answer.
    bool finished = false;
    for (int reads = 0; reads < 200 && !finished; ++reads) {
        FailingSource source(hello, reads);
        Package package;
        Error error;
        if (package.open(&source, packageOnly, &error)) finished = true;
        else CHECK_IN(error.code + ": " + error.message, error.code == "io-error");
    }
    CHECK(finished);

    // 7. A file that changes between open() and extract() is caught by extract.
    {
        std::string data = hello;
        MemorySource source(data.data(), data.size());
        Package package;
        Error error;
        CHECK(package.open(&source, packageOnly, &error));
        // The last entry is stored, so its bytes sit unchanged just before the directory.
        size_t at = data.rfind("post-remove: done");
        CHECK(at != std::string::npos);
        data[at] = 'P';
        testutil::StringSink sink;
        CHECK(!package.extract("scripts/post-remove.sh", &sink, &error) && error.code == "bad-archive");
    }

    printf("test_package: ok (%lu good, %lu bad, %lu of %lu one-byte changes made no difference)\n",
           (unsigned long)good.size(), (unsigned long)bad.size(), (unsigned long)accepted, (unsigned long)hello.size());
    return 0;
}

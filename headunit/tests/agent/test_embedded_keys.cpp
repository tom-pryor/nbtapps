// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/embedded_keys.hpp: the package key that every nbtagent trusts with no option given.
//   - there is one, marked as built in, and it is the official package key, told here by its fingerprint: a byte
//     of the table changed by mistake fails this test, where the agent would only refuse every official package
//   - it is not a key of tests/keys/, whose private halves are public (vectors.json has the two package keys)
//
// usage: test_embedded_keys [testdata folder]     (default build/testdata)

#include "../../agent/embedded_keys.hpp"

#include <string.h>

#include <string>
#include <vector>

#include "../../agent/hex.hpp"
#include "../../agent/json.hpp"
#include "../../agent/sha256.hpp"
#include "test_util.hpp"

using namespace nbt;
using testutil::readFile;

// What `nbtpkg.py keygen` printed for nbtapps-official-package, and the comment line of the catalogue repo's
// keys/package.pub: the first 16 hex characters of the SHA-256 of the key.
static const char kOfficialFingerprint[] = "d0416126bb842c92";

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "build/testdata";

    std::vector<TrustedKey> keys;
    addEmbeddedKeys(&keys);
    CHECK(keys.size() == 1);
    CHECK(keys[0].embedded && keys[0].name == "NBTApps");
    CHECK(memcmp(keys[0].key, kOfficialPackageKey, sizeof keys[0].key) == 0);

    uint8_t digest[kSha256Bytes];
    sha256(keys[0].key, sizeof keys[0].key, digest);
    CHECK_IN(toHex(digest, sizeof digest), toHex(digest, sizeof digest).substr(0, 16) == kOfficialFingerprint);

    // Called again it adds the same key again and takes nothing away: the caller owns the list.
    addEmbeddedKeys(&keys);
    CHECK(keys.size() == 2 && memcmp(keys[0].key, keys[1].key, sizeof keys[0].key) == 0);

    json::Document vectors;
    std::string why;
    CHECK_IN(why, vectors.parse(readFile(dir + "/vectors.json"), &why));
    const char* test[] = {"package", "other"};
    for (size_t i = 0; i < sizeof test / sizeof test[0]; ++i) {
        TrustedKey k = testutil::key(vectors.root().find("keys").find(test[i]).string(), test[i], false);
        CHECK_IN(test[i], memcmp(k.key, keys[0].key, sizeof k.key) != 0);
    }

    printf("test_embedded_keys: ok\n");
    return 0;
}

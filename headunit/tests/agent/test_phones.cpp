// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/phones.cpp: the record of paired phones and their keys.
//
// usage: test_phones [work folder]

#include "../../agent/phones.hpp"

#include <string.h>
#include <sys/stat.h>

#include "../../agent/fsutil.hpp"
#include "test_util.hpp"

using namespace nbt;

static PairedPhone phoneOf(char digit, const char* name, int64_t pairedAt) {
    PairedPhone p;
    p.id = std::string(32, digit);
    p.name = name;
    p.pairedAt = pairedAt;
    memset(p.key, digit, sizeof p.key);
    return p;
}

static bool same(const PairedPhone& a, const PairedPhone& b) {
    return a.id == b.id && a.name == b.name && a.pairedAt == b.pairedAt && memcmp(a.key, b.key, sizeof a.key) == 0;
}

int main(int argc, char** argv) {
    std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host") + "/work-phones";
    std::string why, path = work + "/phones.json";
    CHECK_IN(why, fs::removeTree(work, &why) && fs::makeDirs(work, &why));
    fs::setSyncing(false);

    {   // No file: no phones, and none is written by looking.
        PhoneStore store;
        CHECK_IN(why, store.load(path, &why) && store.all().empty() && store.find(std::string(32, '1')) == 0);
        CHECK(fs::kindOf(path) == fs::kNone);
    }
    {   // A pairing is on disk before put() returns, for the agent's user alone, and is there for the next agent.
        PhoneStore store;
        CHECK(store.load(path, &why));
        PairedPhone one = phoneOf('1', "Pixel 9", 1794400000), two = phoneOf('2', "Tom\xE2\x80\x99s \"phone\"\\", 5);
        CHECK_IN(why, store.put(one, &why) && store.put(two, &why));
        struct stat st;
        CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        CHECK(fs::kindOf(fs::tempIn(work)) == fs::kNone);
        PhoneStore next;
        CHECK_IN(why, next.load(path, &why) && next.all().size() == 2);
        CHECK(same(next.all()[0], one) && same(next.all()[1], two) && same(*next.find(two.id), two));

        // The same phone again: one pairing, with the new key, and now the newest.
        PairedPhone again = phoneOf('1', "Pixel 9 Pro", 7);
        memset(again.key, 0xEE, sizeof again.key);
        CHECK(store.put(again, &why) && store.all().size() == 2 && same(store.all()[0], two) && same(store.all()[1], again));

        // Forgetting: one that is there, one that is not, and all.
        size_t count = 99;
        CHECK(store.forget(std::string(32, '9'), &count, &why) && count == 0 && store.all().size() == 2);
        CHECK(store.forget(two.id, &count, &why) && count == 1 && store.all().size() == 1 && store.find(two.id) == 0);
        CHECK(next.load(path, &why) && next.all().size() == 1 && same(next.all()[0], again));
        CHECK(store.forgetAll(&count, &why) && count == 1 && store.all().empty());
        CHECK(next.load(path, &why) && next.all().empty());
    }
    {   // At most 16: the oldest pairing makes room.
        PhoneStore store;
        CHECK(store.load(path, &why));
        const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < kMaxPairedPhones; ++i) CHECK(store.put(phoneOf(digits[i], "phone", (int64_t)i), &why));
        PairedPhone extra;
        extra.id = "f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0";
        extra.name = "one more";
        CHECK(store.put(extra, &why) && store.all().size() == kMaxPairedPhones);
        CHECK(store.find(std::string(32, '0')) == 0 && store.find(std::string(32, '1')) != 0 && store.find(extra.id) != 0);
        PhoneStore next;
        CHECK(next.load(path, &why) && next.all().size() == kMaxPairedPhones && next.all()[kMaxPairedPhones - 1].id == extra.id);
    }
    {   // A file that is not what the store writes: no phones, and the reason. The next change replaces it.
        std::vector<PairedPhone> good(1, phoneOf('1', "Pixel 9", 3));
        std::string text = PhoneStore::encode(good);
        std::vector<PairedPhone> read;
        CHECK_IN(why, PhoneStore::decode(text, &read, &why) && read.size() == 1 && same(read[0], good[0]));
        const char* key = "3131313131313131313131313131313131313131313131313131313131313131";
        std::string entry = std::string("{\"phoneId\":\"11111111111111111111111111111111\",\"name\":\"n\",\"key\":\"") + key + "\",\"pairedAt\":3}";
        CHECK_IN(why, PhoneStore::decode("{\"version\":1,\"phones\":[" + entry + "]}", &read, &why) && read.size() == 1);
        std::string seventeen;
        for (int i = 0; i < 17; ++i) seventeen += (i == 0 ? "" : ",") + entry;
        const std::string bad[] = {
            "",
            "not json",
            "{}",
            "{\"version\":2,\"phones\":[]}",
            "{\"version\":1,\"phones\":{}}",
            "{\"version\":1,\"phones\":[{}]}",
            "{\"version\":1,\"phones\":[" + entry + "," + entry + "]}",   // the same phone twice
            "{\"version\":1,\"phones\":[" + seventeen + "]}",
            "{\"version\":1,\"phones\":[{\"phoneId\":\"1111111111111111111111111111111\",\"name\":\"n\",\"key\":\"" + std::string(key) + "\",\"pairedAt\":3}]}",
            "{\"version\":1,\"phones\":[{\"phoneId\":\"1111111111111111111111111111111A\",\"name\":\"n\",\"key\":\"" + std::string(key) + "\",\"pairedAt\":3}]}",
            "{\"version\":1,\"phones\":[{\"phoneId\":\"11111111111111111111111111111111\",\"name\":\"\",\"key\":\"" + std::string(key) + "\",\"pairedAt\":3}]}",
            "{\"version\":1,\"phones\":[{\"phoneId\":\"11111111111111111111111111111111\",\"name\":\"" + std::string(65, 'n') + "\",\"key\":\"" + std::string(key) + "\",\"pairedAt\":3}]}",
            "{\"version\":1,\"phones\":[{\"phoneId\":\"11111111111111111111111111111111\",\"name\":\"n\",\"key\":\"3131\",\"pairedAt\":3}]}",
            "{\"version\":1,\"phones\":[{\"phoneId\":\"11111111111111111111111111111111\",\"name\":\"n\",\"key\":\"" + std::string(key) + "\",\"pairedAt\":-1}]}",
            "{\"version\":1,\"phones\":[{\"phoneId\":\"11111111111111111111111111111111\",\"name\":\"n\",\"key\":\"" + std::string(key) + "\"}]}",
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            why.clear();
            read.assign(1, good[0]);
            CHECK_IN(bad[i], !PhoneStore::decode(bad[i], &read, &why) && read.empty() && !why.empty());
            CHECK_IN(why, fs::writeFile(path, bad[i], 0600, &why));
            PhoneStore store;
            why.clear();
            CHECK_IN(bad[i], !store.load(path, &why) && store.all().empty() && !why.empty());
            // The store is usable all the same.
            CHECK_IN(why, store.put(good[0], &why));
            PhoneStore next;
            CHECK_IN(why, next.load(path, &why) && next.all().size() == 1 && same(next.all()[0], good[0]));
        }
        // A file too long to be one of its own is not read into memory at all.
        CHECK_IN(why, fs::writeFile(path, std::string(1u << 20, ' '), 0600, &why));
        PhoneStore store;
        CHECK(!store.load(path, &why) && store.all().empty());
    }
    {   // A folder that cannot be written: the pairing fails, and the store is as it was.
        PhoneStore store;
        CHECK(store.load(work + "/no-such-folder/phones.json", &why));
        size_t count = 0;
        CHECK(!store.put(phoneOf('1', "Pixel 9", 3), &why) && store.all().empty() && !why.empty());
        CHECK(!store.forgetAll(&count, &why));
    }

    fs::removeTree(work, 0);
    printf("test_phones: ok\n");
    return 0;
}

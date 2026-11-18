// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/presence.cpp: which of the car's phones are there, from the car's device lists. All
// addresses are made up.

#include "../../agent/presence.hpp"

#include "test_util.hpp"

using namespace nbt;
using namespace nbt::btspp;

static BtDevice device(const char* address, uint32_t connection, uint32_t security) {
    BtDevice d;
    d.address = address;
    d.connection = connection;
    d.security = security;
    return d;
}

static const Presence::Change* changeOf(const std::vector<Presence::Change>& changes, unsigned number) {
    for (size_t i = 0; i < changes.size(); ++i) {
        if (changes[i].number == number) return &changes[i];
    }
    return 0;
}

int main() {
    {   // nothing known before the first list
        Presence p;
        CHECK(!p.known() && p.phones().empty() && p.find("00:11:22:33:44:55") == 0);
        CHECK(p.presentCount() == 0 && p.dialableCount() == 0);
    }
    {   // a first list: every phone is new, gets a number, and is there with what the list says
        Presence p;
        std::vector<BtDevice> list;
        list.push_back(device("00:11:22:33:44:55", kConnConnected, 7));
        list.push_back(device("66:77:88:99:AA:BB", kConnInRange, 8));
        list.push_back(device("CC:DD:EE:FF:00:11", kConnServiceConnected, 9));
        list.push_back(device("", kConnConnected, 7));                          // no address: not a phone to keep
        list.push_back(device("00:11:22:33:44:55", kConnNotInRange, 7));        // the same phone twice: the first
        std::vector<Presence::Change> changes;
        p.update(list, 1000, &changes);
        CHECK(p.known() && p.phones().size() == 3 && changes.size() == 3);
        CHECK(p.phones()[0].number == 1 && p.phones()[1].number == 2 && p.phones()[2].number == 3);
        CHECK(changes[0].added && changes[0].connectionChanged && changes[0].securityChanged && changes[0].connection == 2);
        const Presence::Phone* one = p.find("001122334455");   // any form
        CHECK(one != 0 && one->connection == kConnConnected && one->security == 7 && one->connectionAt == 1000);
        CHECK(Presence::present(*one) && Presence::dialable(*one));
        CHECK(!Presence::present(*p.find("66:77:88:99:aa:bb")) && !Presence::dialable(*p.find("66:77:88:99:AA:BB")));
        CHECK(Presence::present(*p.find("CC:DD:EE:FF:00:11")) && !Presence::dialable(*p.find("CC:DD:EE:FF:00:11")));
        CHECK(p.presentCount() == 2 && p.dialableCount() == 1);

        // the next list: what changed, and when, phone by phone; what did not keeps its time
        list.clear();
        list.push_back(device("00:11:22:33:44:55", kConnServiceConnected, 7));   // connection only
        list.push_back(device("66:77:88:99:AA:BB", kConnConnected, 8));          // connection only
        list.push_back(device("CC:DD:EE:FF:00:11", kConnServiceConnected, 7));   // security only
        changes.clear();
        p.update(list, 5000, &changes);
        CHECK(changes.size() == 3);
        const Presence::Change* c = changeOf(changes, 1);
        CHECK(c != 0 && !c->added && c->connectionChanged && !c->securityChanged && c->connectionBefore == 2 && c->connection == 3);
        c = changeOf(changes, 3);
        CHECK(c != 0 && !c->connectionChanged && c->securityChanged && c->securityBefore == 9 && c->security == 7);
        CHECK(p.find("00:11:22:33:44:55")->connectionAt == 5000 && p.find("00:11:22:33:44:55")->securityAt == 1000);
        CHECK(p.find("CC:DD:EE:FF:00:11")->connectionAt == 1000 && p.find("CC:DD:EE:FF:00:11")->securityAt == 5000);
        CHECK(p.dialableCount() == 3);

        // the same list again: no change at all
        changes.clear();
        p.update(list, 6000, &changes);
        CHECK(changes.empty() && p.find("00:11:22:33:44:55")->connectionAt == 5000);

        // a phone that leaves the list, and a new one: the new one gets the next number
        list.erase(list.begin() + 1);
        list.push_back(device("12:34:56:78:9A:BC", kConnConnected, 7));
        changes.clear();
        p.update(list, 7000, &changes);
        CHECK(changes.size() == 2 && p.phones().size() == 3 && p.find("66:77:88:99:AA:BB") == 0);
        c = changeOf(changes, 2);
        CHECK(c != 0 && c->removed && c->connectionChanged && c->connectionBefore == kConnConnected);
        c = changeOf(changes, 4);
        CHECK(c != 0 && c->added);

        // the service goes: nothing known; numbers stay for the life of the process
        p.forget();
        CHECK(!p.known() && p.phones().empty() && p.presentCount() == 0);
        CHECK(p.number("66:77:88:99:AA:BB") == 2 && p.number("12:34:56:78:9a:bc") == 4 && p.number("01:02:03:04:05:06") == 5);
        list.clear();
        list.push_back(device("66:77:88:99:AA:BB", kConnConnected, 7));
        changes.clear();
        p.update(list, 9000, &changes);
        CHECK(changes.size() == 1 && changes[0].added && changes[0].number == 2 && p.find("66:77:88:99:AA:BB")->number == 2);
    }
    {   // pairedSecurity: 7 and 8 only
        CHECK(pairedSecurity(7) && pairedSecurity(8) && !pairedSecurity(0) && !pairedSecurity(9) && !pairedSecurity(6));
    }
    printf("test_presence: ok\n");
    return 0;
}

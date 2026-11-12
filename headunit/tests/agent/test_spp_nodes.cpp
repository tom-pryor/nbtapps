// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/spp_nodes.hpp: the rule that decides which /dev/btsppN link is ours. The times are invented.

#include "../../agent/spp_nodes.hpp"

#include <stdio.h>

#include "test_util.hpp"

using namespace nbt::btspp;

static void none(bool p[NodeTracker::kNodes]) {
    for (int i = 0; i < NodeTracker::kNodes; ++i) p[i] = false;
}

int main() {
    bool p8[NodeTracker::kNodes];
    {   // a node that exists at the start is never ours, even after a request; once gone and back with one, it is
        NodeTracker t(15, 2);
        none(p8); p8[0] = true;
        t.baseline(p8);
        CHECK(t.state(0) == NodeTracker::kForeign);
        t.onPhoneRequest(100);
        CHECK(t.scan(p8, 100, false) == -1 && t.state(0) == NodeTracker::kForeign);
        none(p8);
        CHECK(t.scan(p8, 101, false) == -1 && t.state(0) == NodeTracker::kAbsent);
        p8[0] = true;
        CHECK(t.scan(p8, 102, false) == 0 && t.state(0) == NodeTracker::kOurs);   // the request from t=100 is still fresh
        CHECK(t.scan(p8, 103, false) == -1 && t.state(0) == NodeTracker::kOurs);  // returned once only
    }
    {   // request first, node second: ours. One request accounts for one link.
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        t.onPhoneRequest(200);
        p8[1] = true;
        CHECK(t.scan(p8, 203, false) == 1);
        p8[2] = true;                                       // a second node, no new request
        CHECK(t.scan(p8, 204, false) == -1 && t.state(2) == NodeTracker::kPending);
        CHECK(t.scan(p8, 207, false) == -1 && t.state(2) == NodeTracker::kForeign);   // grace over
        t.onPhoneRequest(208);                              // too late for node 2: it stays someone else's
        CHECK(t.scan(p8, 208, false) == -1 && t.state(2) == NodeTracker::kForeign);
    }
    {   // node first, request within the grace time: ours
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        p8[0] = true;
        CHECK(t.scan(p8, 300, false) == -1 && t.state(0) == NodeTracker::kPending);
        t.onPhoneRequest(301);
        CHECK(t.scan(p8, 301, false) == 0);
    }
    {   // a request older than the window does not count
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        t.onPhoneRequest(400);
        p8[0] = true;
        CHECK(t.scan(p8, 416, false) == -1 && t.state(0) == NodeTracker::kPending);
        CHECK(t.scan(p8, 419, false) == -1 && t.state(0) == NodeTracker::kForeign);
    }
    {   // a node with no request at all (a link the car opened) is left alone; `any` takes it
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        p8[0] = true;
        CHECK(t.scan(p8, 500, false) == -1);
        CHECK(t.scan(p8, 503, false) == -1 && t.state(0) == NodeTracker::kForeign);
        NodeTracker a(15, 2);
        none(p8);
        a.baseline(p8);
        p8[3] = true;
        CHECK(a.scan(p8, 500, true) == 3 && a.state(3) == NodeTracker::kOurs);
    }
    {   // two new nodes in one scan with one request: only the first is ours
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        t.onPhoneRequest(600);
        p8[0] = p8[1] = true;
        CHECK(t.scan(p8, 600, false) == 0);
        CHECK(t.state(1) == NodeTracker::kPending);
        CHECK(t.state(-1) == NodeTracker::kAbsent && t.state(NodeTracker::kNodes) == NodeTracker::kAbsent);
    }

    {   // a request for another service: its node is someone else's at once, and says why
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        t.onPhoneRequest(700, false);
        p8[0] = true;
        CHECK(t.scan(p8, 700, false) == -1 && t.state(0) == NodeTracker::kForeign && t.otherService(0));
        p8[1] = true;                                       // nothing was asked for with this one
        CHECK(t.scan(p8, 701, false) == -1 && t.state(1) == NodeTracker::kPending);
        CHECK(t.scan(p8, 704, false) == -1 && t.state(1) == NodeTracker::kForeign && !t.otherService(1));
        none(p8);
        CHECK(t.scan(p8, 705, false) == -1 && !t.otherService(0));
    }
    {   // requests are matched to nodes in the order they came, whoever they are for
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        t.onPhoneRequest(800, false);
        t.onPhoneRequest(800, true);
        t.onPhoneRequest(800, false);
        p8[0] = true;
        CHECK(t.scan(p8, 800, false) == -1 && t.otherService(0));
        p8[1] = true;
        CHECK(t.scan(p8, 801, false) == 1 && t.state(1) == NodeTracker::kOurs);
        p8[2] = true;
        CHECK(t.scan(p8, 801, false) == -1 && t.otherService(2));
        NodeTracker u(15, 2);                               // all three nodes in one scan
        none(p8);
        u.baseline(p8);
        u.onPhoneRequest(810, true);
        u.onPhoneRequest(810, false);
        p8[0] = p8[1] = p8[2] = true;
        CHECK(u.scan(p8, 810, false) == 0 && u.otherService(1) && u.state(2) == NodeTracker::kPending);
    }
    {   // another app that asks for another service again and again: none of its links is ours
        NodeTracker t(15, 2);
        none(p8); p8[0] = true;
        t.baseline(p8);
        none(p8);
        CHECK(t.scan(p8, 2, false) == -1);
        const uint32_t at[] = {5, 15, 25, 27, 35, 45};
        for (int i = 0; i < 6; ++i) {
            t.onPhoneRequest(at[i], false);
            p8[0] = true;
            CHECK(t.scan(p8, at[i], false) == -1 && t.state(0) == NodeTracker::kForeign);
            p8[0] = false;
            CHECK(t.scan(p8, at[i] + 1, false) == -1);
        }
        t.onPhoneRequest(47, true);                         // and then the echo app
        p8[1] = true;
        CHECK(t.scan(p8, 47, false) == 1);
    }
    {   // a refused request brings no link: the next node is not its
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        t.onPhoneRequest(900, false);
        t.onRefused(false);
        t.onPhoneRequest(901, true);
        p8[0] = true;
        CHECK(t.scan(p8, 901, false) == 0);
        t.onPhoneRequest(902, true);
        t.onRefused(false);                                 // none of that kind is kept: nothing happens
        t.onRefused(true);
        p8[1] = true;
        CHECK(t.scan(p8, 902, false) == -1 && t.state(1) == NodeTracker::kPending);
        CHECK(t.scan(p8, 905, false) == -1 && t.state(1) == NodeTracker::kForeign);
    }
    {   // a request for another service that is too old does not claim our node; more requests than are kept
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        t.onPhoneRequest(1000, false);
        t.onPhoneRequest(1016, true);
        p8[0] = true;
        CHECK(t.scan(p8, 1016, false) == 0);
        NodeTracker u(15, 2);
        none(p8);
        u.baseline(p8);
        for (int i = 0; i < NodeTracker::kRequests + 3; ++i) u.onPhoneRequest(1100, false);
        u.onPhoneRequest(1100, true);                       // the oldest fall out; ours is still the last
        for (int i = 0; i < NodeTracker::kRequests - 1; ++i) {
            p8[i] = true;
            CHECK(u.scan(p8, 1100, false) == -1 && u.otherService(i));
        }
        p8[NodeTracker::kRequests - 1] = true;
        CHECK(u.scan(p8, 1100, false) == NodeTracker::kRequests - 1);
    }
    {   // `any` takes a node whatever was asked for
        NodeTracker t(15, 2);
        none(p8);
        t.baseline(p8);
        t.onPhoneRequest(1200, false);
        p8[0] = true;
        CHECK(t.scan(p8, 1200, true) == 0 && t.state(0) == NodeTracker::kOurs);
    }

    printf("test_spp_nodes: ok\n");
    return 0;
}

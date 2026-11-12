// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_SPP_NODES_HPP
#define NBT_AGENT_SPP_NODES_HPP

// Which of the car's Bluetooth serial nodes holds a link of ours. No devices and no clock of its own: the caller
// says which nodes exist and what time it is. Plain C++98, host-tested in tests/agent/test_spp_nodes.cpp.

#include <stdint.h>

namespace nbt {
namespace btspp {

// ---- which link is ours --------------------------------------------------------------------------------------
// The car numbers its Bluetooth serial links /dev/btspp0, 1, ... in connect order, and other users share them: the
// car's own 'SPP' service, which phone apps connect to on their own and again and again, links the car opens
// itself, and any other program that registered a UUID. The node name says nothing about who a link belongs to. So:
//   * a node that exists at the start is someone else's until it disappears
//   * the phone's requests (ServiceRequest informs) are kept in the order they came, each for our service or for
//     another one, for the length of the window; a request the car refused is taken out again
//   * a node that appears later belongs to the oldest request still kept: ours if that request named our service,
//     someone else's if it named another. One request accounts for one link
//   * a node with no request waits the grace time for one, and is then someone else's
//   * anything that is not ours is left alone: never opened, never read
// `any` takes every new node, whatever was asked for.
// A ServiceRequestComplete follows a request for the same link, sometimes after the node is there, so it is not
// counted as a request of its own.
class NodeTracker {
public:
    enum { kNodes = 8, kRequests = 8 };
    enum State { kAbsent, kForeign, kPending, kOurs };

    explicit NodeTracker(uint32_t windowS = 15, uint32_t graceS = 2)
        : windowS_(windowS ? windowS : 1), graceS_(graceS), nReq_(0) {
        for (int i = 0; i < kNodes; ++i) { state_[i] = kAbsent; since_[i] = 0; other_[i] = false; }
    }

    // The nodes that exist before the service is registered.
    void baseline(const bool present[kNodes]) {
        for (int i = 0; i < kNodes; ++i) { state_[i] = present[i] ? kForeign : kAbsent; other_[i] = false; }
    }

    // A ServiceRequest inform arrived: the phone asked for a link to our service (`ours`) or to another one.
    void onPhoneRequest(uint32_t now, bool ours = true) {
        if (nReq_ == kRequests) drop(0);
        req_[nReq_].at = now;
        req_[nReq_].ours = ours;
        ++nReq_;
    }

    // A ServiceRequestError inform arrived: the car refused the oldest such request, and no link comes of it.
    void onRefused(bool ours) {
        for (int i = 0; i < nReq_; ++i) {
            if (req_[i].ours == ours) { drop(i); return; }
        }
    }

    // Call with the nodes that exist now. Returns the node that has just become ours (open it), or -1.
    int scan(const bool present[kNodes], uint32_t now, bool any) {
        while (nReq_ && !fresh(req_[0], now)) drop(0);
        int take = -1;
        for (int i = 0; i < kNodes; ++i) {
            if (!present[i]) { state_[i] = kAbsent; continue; }
            if (state_[i] == kAbsent) { state_[i] = kPending; since_[i] = now; other_[i] = false; }
            if (state_[i] != kPending) continue;
            if (any || (nReq_ && req_[0].ours)) {
                if (take < 0) {
                    if (nReq_) drop(0);
                    state_[i] = kOurs;
                    take = i;
                } else if (now - since_[i] > graceS_) {
                    state_[i] = kForeign;
                }
            } else if (nReq_) {
                drop(0);
                state_[i] = kForeign;
                other_[i] = true;
            } else if (now - since_[i] > graceS_) {
                state_[i] = kForeign;
            }
        }
        return take;
    }

    State state(int i) const { return (i >= 0 && i < kNodes) ? state_[i] : kAbsent; }
    // Whether node i is someone else's because the phone asked for another service with it.
    bool otherService(int i) const { return i >= 0 && i < kNodes && state_[i] == kForeign && other_[i]; }

private:
    struct Request {
        uint32_t at;
        bool ours;
    };

    bool fresh(const Request& r, uint32_t now) const {
        return (int32_t)(now - r.at) >= 0 && (now - r.at) <= windowS_;
    }
    void drop(int i) {
        for (int k = i; k + 1 < nReq_; ++k) req_[k] = req_[k + 1];
        --nReq_;
    }

    uint32_t windowS_, graceS_;
    Request req_[kRequests];
    int nReq_;
    State state_[kNodes];
    uint32_t since_[kNodes];
    bool other_[kNodes];
};

}  // namespace btspp
}  // namespace nbt

#endif

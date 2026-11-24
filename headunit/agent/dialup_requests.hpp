// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_DIALUP_REQUESTS_HPP
#define NBT_AGENT_DIALUP_REQUESTS_HPP

// The knock by dial-up (constants.txt, serial.knockDialupUuid): the phones' requests for the car's dial-up
// networking service that are pending, and the agent's backstop refusal of each. No devices and no clock of its own:
// the Bluetooth serial listener (serial_link.hpp) feeds it what the car says, with the time, and does what it says.
// Plain C++98, host-tested in tests/agent/test_dial_rules.cpp.
//
// A phone's request for that service is that phone's knock. The car's own software refuses it within milliseconds,
// and the stack then says that the request has ended (ServiceRequestError). But the car's software sends one refusal
// at a time and drops one that comes while its last is unanswered, and the stack never ends a pending request by
// itself: the phone then waits a minute, and its next request is refused unheard. So every request is followed:
//
//   * heard(): a request of the phone's. A second one while its first is pending adds nothing (the stack does not
//     raise one, but should one come, it is the same request)
//   * ended(): the stack says it has ended. Nothing is sent for it, and a refusal of the agent's that is due for it is
//     not sent: never one for a request that has ended
//   * refusalDue(): one the car has not ended in refuseAfterMs gets the agent's refusal (ServiceReject), never at
//     once, one at a time: the stack has one busy slot for every client's refusals, and a second one in flight is
//     answered BUSY, as one that met the car's own would be
//   * answered(): BUSY (2), a rejection of the request, or none in replyMs (refusalLost()): sent again retryMs later,
//     `retries` times at most, and then given up. Any other answer ends the request: 5 is the one the stack gives,
//     whether or not the refusal worked (the car's dial-up service does not tell the stack how it went)
//
// What ends a request, whichever way, is the moment its knock is taken (DialRules::knocked()): the car does not dial
// the phone while the phone's own connect to the dial-up service is still waiting for its answer. clear() forgets
// everything: the car's Bluetooth switched off, or its Bluetooth service gone, ends every request in the stack.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stdint.h>

#include <string>
#include <vector>

#include "spp_service.hpp"

namespace nbt {

class DialupRequests {
public:
    struct Timing {
        int64_t refuseAfterMs, retryMs, replyMs;
        unsigned retries;
        Timing() : refuseAfterMs(2000), retryMs(100), replyMs(3000), retries(5) {}
    };
    // What became of the agent's refusal that was out.
    enum Answer {
        kNoneOut,     // none was out: nothing
        kEnded,       // the request ends now: its knock goes
        kAfterEnd,    // the request had ended already, by the stack's word: nothing more
        kRetry,       // BUSY, rejected or unanswered: sent again retryMs later
        kGaveUp       // so, `retries` times: given up. The request may stay pending in the stack; its knock goes
    };

    explicit DialupRequests(const Timing& t = Timing()) : t_(t), outOpen_(false), outAt_(0), sent_(0), gaveUp_(0) {}

    // A request of the phone at `address`, heard at `now`. False if one of that phone's is pending already.
    bool heard(const std::string& address, int64_t now) {
        std::string key = btspp::addressKey(address);
        if (key.empty() || find(key) >= 0) return false;
        Request r;
        r.key = key;
        r.address = address;
        r.heardAt = now;
        r.dueAt = now + t_.refuseAfterMs;
        r.tries = 0;
        pending_.push_back(r);
        return true;
    }
    // The stack says that the phone's request has ended. True, with when it was heard, if it was pending: its knock
    // goes now. A refusal of the agent's that is out for it stays out until its answer, which then says nothing more.
    bool ended(const std::string& address, int64_t* heardAt) {
        int i = find(btspp::addressKey(address));
        if (i < 0) return false;
        *heardAt = pending_[(size_t)i].heardAt;
        pending_.erase(pending_.begin() + i);
        return true;
    }
    // The refusal to send now: none while one of the agent's is out; the request whose time has come first. It counts
    // as out from here (`now`).
    bool refusalDue(int64_t now, std::string* address) {
        if (outOpen_) return false;
        int best = -1;
        for (size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].dueAt > now) continue;
            if (best < 0 || pending_[i].dueAt < pending_[(size_t)best].dueAt) best = (int)i;
        }
        if (best < 0) return false;
        Request& r = pending_[(size_t)best];
        ++r.tries;
        ++sent_;
        outOpen_ = true;
        outKey_ = r.key;
        outAt_ = now;
        *address = r.address;
        return true;
    }
    // The answer to the refusal that is out: the stack's error code, or btspp::kNoError for a rejection or a request
    // that could not be sent. *address, *heardAt: the request's, for kEnded and kGaveUp.
    Answer answered(uint32_t error, int64_t now, std::string* address, int64_t* heardAt) {
        if (!outOpen_) return kNoneOut;
        outOpen_ = false;
        int i = find(outKey_);
        if (i < 0) return kAfterEnd;
        Request& r = pending_[(size_t)i];
        *address = r.address;
        *heardAt = r.heardAt;
        if (error == 2 || error == btspp::kNoError) {
            if (r.tries <= t_.retries) {
                r.dueAt = now + t_.retryMs;
                return kRetry;
            }
            pending_.erase(pending_.begin() + i);
            ++gaveUp_;
            return kGaveUp;
        }
        pending_.erase(pending_.begin() + i);
        return kEnded;
    }
    // A refusal out with no answer in replyMs: answered() as with no error code, and its late answer, should it come,
    // is passed over (kNoneOut). False if none is out or its time has not run.
    bool refusalLost(int64_t now, Answer* what, std::string* address, int64_t* heardAt) {
        if (!outOpen_ || now - outAt_ < t_.replyMs) return false;
        *what = answered(btspp::kNoError, now, address, heardAt);
        return true;
    }
    void clear() {
        pending_.clear();
        outOpen_ = false;
    }
    // Whether a request is pending or a refusal out: the listener looks often then.
    bool busy() const { return !pending_.empty() || outOpen_; }
    bool refusalOut() const { return outOpen_; }
    size_t pending() const { return pending_.size(); }
    unsigned refusalsSent() const { return sent_; }
    unsigned givenUp() const { return gaveUp_; }

private:
    struct Request {
        std::string key, address;
        int64_t heardAt, dueAt;
        unsigned tries;
    };
    int find(const std::string& key) const {
        for (size_t i = 0; i < pending_.size(); ++i)
            if (pending_[i].key == key) return (int)i;
        return -1;
    }

    Timing t_;
    std::vector<Request> pending_;
    bool outOpen_;
    std::string outKey_;
    int64_t outAt_;
    unsigned sent_, gaveUp_;
};

}  // namespace nbt

#endif

// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#include "spp_service.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

namespace nbt {
namespace btspp {

namespace {

double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

}  // namespace

// ---- DsiGapClient ------------------------------------------------------------------------------------------------

DsiGapClient::DsiGapClient() : events_(NULL), verMajor_(kGapMajor), verMinor_(kGapMinor) {
    pthread_mutex_init(&tx_, NULL);
}

DsiGapClient::~DsiGapClient() { pthread_mutex_destroy(&tx_); }

void DsiGapClient::configure(int verMajor, int verMinor, bool verbose) {
    verMajor_ = verMajor;
    verMinor_ = verMinor;
    dsi_.setVerbose(verbose);
}

bool DsiGapClient::open(Events* events) {
    events_ = events;
    dsi_.setCallbacks(&DsiGapClient::onAttached, &DsiGapClient::onData, &DsiGapClient::onDetached, this);
    return dsi_.open(kGapInterface, kGapRole, verMajor_, verMinor_);
}

bool DsiGapClient::request(uint32_t id, const std::vector<uint8_t>& payload) {
    pthread_mutex_lock(&tx_);
    bool ok = dsi_.sendRequest(id, payload);
    pthread_mutex_unlock(&tx_);
    return ok;
}

bool DsiGapClient::subscribe(uint32_t informId) {
    pthread_mutex_lock(&tx_);
    bool ok = dsi_.setNotification(informId);
    pthread_mutex_unlock(&tx_);
    return ok;
}

std::string DsiGapClient::describe() const {
    char b[96];
    snprintf(b, sizeof(b), "%s.%s %d.%d", kGapRole, kGapInterface, verMajor_, verMinor_);
    return b;
}

void DsiGapClient::onAttached(DsiClient*, void* self) { static_cast<DsiGapClient*>(self)->events_->gapAttached(); }

void DsiGapClient::onDetached(DsiClient*, void* self) { static_cast<DsiGapClient*>(self)->events_->gapDetached(); }

void DsiGapClient::onData(DsiClient*, uint32_t type, uint32_t id, uint32_t, const uint8_t* payload, uint32_t len,
                          void* self) {
    static_cast<DsiGapClient*>(self)->events_->gapMessage(type, id, payload, len);
}

// ---- SppService --------------------------------------------------------------------------------------------------

SppService::SppService(GapClient* gap, Events* events)
    : gap_(gap), events_(events), type_(0), state_(kIdle), mode_(kModeOff), subscribed_(false), otherSentAt_(0),
      otherReplyS_(kOtherRemovalReplyS) {
    pthread_mutex_init(&mu_, NULL);
}

SppService::~SppService() { pthread_mutex_destroy(&mu_); }

void SppService::configure(const std::string& name, const std::string& uuid, uint32_t type) {
    name_ = name;
    uuid_ = uuid;
    type_ = type;
}

void SppService::say(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    events_->sppSay(line);
}

SppService::State SppService::state() {
    pthread_mutex_lock(&mu_);
    State s = state_;
    pthread_mutex_unlock(&mu_);
    return s;
}

void SppService::set(State s) {
    pthread_mutex_lock(&mu_);
    bool changed = state_ != s;
    state_ = s;
    pthread_mutex_unlock(&mu_);
    if (changed) events_->sppState(s);
}

SppService::Mode SppService::mode() {
    pthread_mutex_lock(&mu_);
    Mode m = mode_;
    pthread_mutex_unlock(&mu_);
    return m;
}

void SppService::setMode(Mode m) {
    pthread_mutex_lock(&mu_);
    mode_ = m;
    pthread_mutex_unlock(&mu_);
}

bool SppService::start() {
    setMode(kModeKeep);
    set(kWaiting);
    return gap_->open(this);
}

bool SppService::startRemoval() {
    setMode(kModeRemoveOnly);
    set(kWaiting);
    return gap_->open(this);
}

bool SppService::remove(const char* why) {
    setMode(kModeOff);
    if (!gap_->attached()) return false;
    set(kRemoveSent);   // before the request goes out: its reply comes on another thread
    if (sendDeregister(why)) return true;
    set(kRemoveFailed);
    return false;
}

void SppService::again() {
    set(kWaiting);
    setMode(kModeKeep);
    if (!gap_->attached()) return;   // gapAttached() goes on from here, when the server is there
    // The server is there. If it has attached since the two lines above, gapAttached() has registered already.
    pthread_mutex_lock(&mu_);
    bool waiting = state_ == kWaiting;
    if (waiting) state_ = kRegSent;
    pthread_mutex_unlock(&mu_);
    if (!waiting) return;
    events_->sppState(kRegSent);
    subscribe();
    sendRegister();
}

// The phones' requests for a link: asked for once for each time the server attaches.
void SppService::subscribe() {
    pthread_mutex_lock(&mu_);
    bool had = subscribed_;
    subscribed_ = true;
    pthread_mutex_unlock(&mu_);
    if (had) return;
    gap_->subscribe(kInfServiceRequest);
    gap_->subscribe(kInfServiceRequestComplete);
    gap_->subscribe(kInfServiceRequestError);
}

bool SppService::removeOther(const std::string& name) {
    if (name == name_ || !isServiceName(name) || !gap_->attached()) return false;
    pthread_mutex_lock(&mu_);
    bool have = otherSent_ == name;
    for (size_t i = 0; i < otherWaiting_.size(); ++i) have = have || otherWaiting_[i] == name;
    if (!have) otherWaiting_.push_back(name);
    pthread_mutex_unlock(&mu_);
    sendNextOther();
    return true;
}

// Sends the next removal that waits, unless one is out already or a removal of our own waits for its reply: the
// replies carry no name, and are told apart only by the order they come in.
void SppService::sendNextOther() {
    std::string lost, next;
    pthread_mutex_lock(&mu_);
    double now = monoNow();
    if (!otherSent_.empty() && now - otherSentAt_ > otherReplyS_) {
        lost = otherSent_;
        otherSent_.clear();
    }
    bool ownOut = state_ == kClearSent || state_ == kRemoveSent;
    if (otherSent_.empty() && !ownOut && !otherWaiting_.empty()) {
        next = otherWaiting_.front();
        otherWaiting_.erase(otherWaiting_.begin());
        otherSent_ = next;
        otherSentAt_ = now;
    }
    pthread_mutex_unlock(&mu_);
    if (!lost.empty()) {
        say("no reply to the removal of '%s' in %.0f s", lost.c_str(), otherReplyS_);
        events_->sppOtherRemoved(lost, kNoError);
    }
    if (next.empty()) return;
    say("SppUuidDeregister name '%s': another program's registration, left behind", next.c_str());
    if (send(kReqSppUuidDeregister, buildDeregister(next))) return;
    pthread_mutex_lock(&mu_);
    if (otherSent_ == next) otherSent_.clear();
    pthread_mutex_unlock(&mu_);
    events_->sppOtherRemoved(next, kNoError);
}

bool SppService::send(uint32_t id, const std::vector<uint8_t>& payload) {
    if (gap_->request(id, payload)) return true;
    say("DSI request 0x%x not sent: the Bluetooth server is not attached", id);
    return false;
}

void SppService::sendRegister() {
    say("SppUuidRegister name '%s', UUID %s, type %u", name_.c_str(), uuid_.c_str(), type_);
    send(kReqSppUuidRegister, buildRegister(name_, uuid_, uuid_, type_));
}

bool SppService::sendDeregister(const char* why) {
    say("SppUuidDeregister name '%s': %s", name_.c_str(), why);
    return send(kReqSppUuidDeregister, buildDeregister(name_));
}

void SppService::gapAttached() {
    say("attached to the Bluetooth server %s", gap_->describe().c_str());
    Mode m = mode();
    if (m == kModeRemoveOnly) {
        set(kRemoveSent);
        sendDeregister("requested");
        return;
    }
    if (m != kModeKeep) return;
    subscribe();
    set(kRegSent);
    sendRegister();
}

void SppService::gapDetached() {
    say("the Bluetooth server detached (its process stopped or restarted): the registration is gone with it");
    pthread_mutex_lock(&mu_);
    bool had = state_ == kRegSent || state_ == kClearSent || state_ == kRetrySent || state_ == kRegistered;
    if (had) state_ = kWaiting;
    subscribed_ = false;
    // Every registration went with the server, the ones that removeOther() was asked to remove among them.
    otherWaiting_.clear();
    otherSent_.clear();
    pthread_mutex_unlock(&mu_);
    if (had) events_->sppState(kWaiting);
}

void SppService::gapMessage(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len) {
    if (isServiceRequestInform(id)) {
        // The phone's name and address are personal data: they stay here, and only the service goes on.
        ServiceRequest q = parseServiceRequest(id, payload, len);
        events_->sppPhone(id == kInfServiceRequest ? kPhoneAsked
                                                   : (id == kInfServiceRequestComplete ? kPhoneLinked : kPhoneRefused),
                          q.service, q.ok);
        return;
    }
    bool rejected = type == kTypeRejectedA || type == kTypeRejectedB;

    // The answer to a removal of another program's registration, if one is out: it went out before any removal of
    // our own that is waiting too, so its answer is the first of the two.
    if ((id == kRspSppUuidDeregister && !rejected) || (id == kReqSppUuidDeregister && rejected)) {
        pthread_mutex_lock(&mu_);
        std::string other = otherSent_;
        otherSent_.clear();
        pthread_mutex_unlock(&mu_);
        if (!other.empty()) {
            uint32_t err = rejected ? kNoError : readError(payload, len);
            if (rejected) say("the Bluetooth server rejected the removal of '%s' (type 0x%x)", other.c_str(), type);
            else say("SppUuidDeregister reply for '%s': %u %s", other.c_str(), err, err == 0 ? "OK" : gapErrorName(err));
            events_->sppOtherRemoved(other, err);
            sendNextOther();
            return;
        }
    }

    State s = state();
    if (id == kRspSppUuidRegister && !rejected) {
        uint32_t err = readError(payload, len);
        say("SppUuidRegister reply: %u %s", err, gapErrorName(err));
        // A reply that comes when no registration is waiting for one (after remove(), say) changes nothing.
        if (s != kRegSent && s != kRetrySent) return;
        if (err == 0) {
            set(kRegistered);
        } else if (s == kRegSent) {
            set(kClearSent);
            sendDeregister("clearing a registration an earlier run left behind, then trying again");
        } else {
            set(kFailed);
        }
        return;
    }
    if (rejected) {
        say("the Bluetooth server rejected the request (type 0x%x, id 0x%x)", type, id);
        if (s == kClearSent) { set(kRetrySent); sendRegister(); }
        else if (s == kRegSent || s == kRetrySent) set(kFailed);
        else if (s == kRemoveSent) set(kRemoveFailed);
        sendNextOther();
        return;
    }
    // The deregister reply: 0x80000044, or the first plain response after the request.
    if ((s == kClearSent || s == kRemoveSent) && (id == kRspSppUuidDeregister || type == kTypeResponse)) {
        uint32_t err = readError(payload, len);
        say("SppUuidDeregister reply (id 0x%x): %u %s", id, err, gapErrorName(err));
        if (s == kClearSent) { set(kRetrySent); sendRegister(); }
        else set(err == 0 ? kRemoved : kRemoveFailed);
        sendNextOther();   // a removal of another program's registration may have waited for this answer
    }
}

}  // namespace btspp
}  // namespace nbt

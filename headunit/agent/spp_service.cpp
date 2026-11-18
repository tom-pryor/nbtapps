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
    // A reply's id would bring every other client's replies of that id as well.
    if (isResponseId(informId)) return false;
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
      otherReplyS_(kOtherRemovalReplyS), unknownLeft_(0), removeError_(kNoError), clearError_(kNoError),
      disconnectOut_(false), refusalAsked_(false), versionAsked_(false), endsLogged_(0), unreadSaid_(0), checkOut_(false), checkSentAt_(0),
      unansweredSaid_(false), restoredRun_(0), oddRun_(false) {
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

uint32_t SppService::removeError() {
    pthread_mutex_lock(&mu_);
    uint32_t e = removeError_;
    pthread_mutex_unlock(&mu_);
    return e;
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
    pthread_mutex_lock(&mu_);
    removeError_ = kNoError;
    pthread_mutex_unlock(&mu_);
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
    if (waiting) {
        state_ = kRegSent;
        clearError_ = kNoError;
    }
    pthread_mutex_unlock(&mu_);
    if (!waiting) return;
    events_->sppState(kRegSent);
    subscribe();
    sendRegister();
}

// The informs: asked for once for each time the server attaches. The phones' requests for a link, and what is
// kept of the car's phones and their links.
void SppService::subscribe() {
    pthread_mutex_lock(&mu_);
    bool had = subscribed_;
    subscribed_ = true;
    pthread_mutex_unlock(&mu_);
    if (had) return;
    subscribeTo(kInfServiceRequest);
    subscribeTo(kInfServiceRequestComplete);
    subscribeTo(kInfServiceRequestError);
    subscribeTo(kInfDeviceListChanged);
    subscribeTo(kInfServiceDisconnected);
    subscribeTo(kInfServiceListChanged);
}

void SppService::subscribeTo(uint32_t informId) {
    // Never a reply's id: see isResponseId().
    if (isResponseId(informId)) {
        say("inform 0x%x is not asked for: it is the id of a reply", informId);
        return;
    }
    if (gap_->subscribe(informId)) return;
    say("the Bluetooth server could not be asked for inform 0x%x: %s", informId,
        gap_->attached() ? "the request could not be sent" : "it is not attached");
}

bool SppService::askVersion() {
    if (!gap_->attached()) return false;
    pthread_mutex_lock(&mu_);
    versionAsked_ = true;
    pthread_mutex_unlock(&mu_);
    if (send(kReqVersionGet, std::vector<uint8_t>())) return true;
    pthread_mutex_lock(&mu_);
    versionAsked_ = false;
    pthread_mutex_unlock(&mu_);
    return false;
}

bool SppService::askDeviceList() {
    if (!gap_->attached()) return false;
    return send(kReqDeviceListGet, std::vector<uint8_t>());
}

bool SppService::askAboutRefused() {
    if (!gap_->attached()) return false;
    pthread_mutex_lock(&mu_);
    refusalAsked_ = true;
    pthread_mutex_unlock(&mu_);
    if (send(kReqDeviceListGet, std::vector<uint8_t>())) return true;
    pthread_mutex_lock(&mu_);
    refusalAsked_ = false;
    pthread_mutex_unlock(&mu_);
    return false;
}

bool SppService::checkRegistration(double replyS) {
    if (!gap_->attached()) return false;
    bool lost = false, sayLost = false;
    pthread_mutex_lock(&mu_);
    double now = monoNow();
    if (checkOut_ && now - checkSentAt_ >= replyS) {
        checkOut_ = false;
        lost = true;
        ++checks_.unanswered;
        sayLost = !unansweredSaid_;
        unansweredSaid_ = true;
    }
    bool go = state_ == kRegistered && !checkOut_;
    if (go) {
        checkOut_ = true;
        checkSentAt_ = now;
        ++checks_.sent;
    }
    pthread_mutex_unlock(&mu_);
    if (lost && sayLost)
        say("the registration check of '%s' had no reply in %.0f s; until one is answered, later ones without a reply "
            "are only counted", name_.c_str(), replyS);
    if (!go) return false;
    // Quietly: the check is not a registration of its own, and goes out every half minute.
    if (gap_->request(kReqSppUuidRegister, buildRegister(name_, uuid_, uuid_, type_))) return true;
    pthread_mutex_lock(&mu_);
    checkOut_ = false;
    --checks_.sent;
    pthread_mutex_unlock(&mu_);
    return false;
}

SppService::CheckCounts SppService::checkCounts() {
    pthread_mutex_lock(&mu_);
    CheckCounts c = checks_;
    pthread_mutex_unlock(&mu_);
    return c;
}

// What a registration check came to: the stack's answer, or kNoError with the type of a rejection.
// A stack that keeps giving the same answer would write a line every round: an answer is said when it is news.
void SppService::checkAnswered(uint32_t err, uint32_t rejectedType) {
    bool odd = rejectedType != 0 || (err != 0 && err != 5 && err != 1 && err != 2);
    bool sayIt = false;
    unsigned restoredRun = 0;
    pthread_mutex_lock(&mu_);
    unansweredSaid_ = false;
    if (odd) {
        ++checks_.other;
        if (oddRun_ && checks_.lastOdd == err && checks_.lastOddRejected == rejectedType) {
            ++checks_.lastOddTimes;
        } else {
            checks_.lastOdd = err;
            checks_.lastOddRejected = rejectedType;
            checks_.lastOddTimes = 1;
            oddRun_ = true;
            sayIt = true;
        }
    } else if (err == 0) {
        ++checks_.restored;
        restoredRun = ++restoredRun_;
        sayIt = restoredRun <= 2;
    } else {
        if (err == 5) ++checks_.present;
        else ++checks_.busy;
        restoredRun_ = 0;
        oddRun_ = false;
    }
    pthread_mutex_unlock(&mu_);
    if (!sayIt) return;
    if (err == 0 && restoredRun == 1) {
        say("the car's Bluetooth had dropped the registration of '%s', as switching Bluetooth off and on in iDrive "
            "does, with no word of it: the registration check found it gone and has registered it again",
            name_.c_str());
    } else if (err == 0) {
        say("the registration of '%s' keeps being dropped: the check has found it gone and registered it again %u "
            "times with no check finding it there in between. From now on this is only counted", name_.c_str(),
            restoredRun);
    } else if (rejectedType != 0) {
        say("the Bluetooth server rejected the registration check (type 0x%x). The same answer again is only "
            "counted", rejectedType);
    } else {
        say("the registration check of '%s' was answered %s. The same answer again is only counted", name_.c_str(),
            registerErrorText(err).c_str());
    }
}

SppService::Heard SppService::heard() {
    pthread_mutex_lock(&mu_);
    Heard h = heard_;
    pthread_mutex_unlock(&mu_);
    return h;
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

namespace {
// After a ServiceDisconnect, this many answers that this class does not know are logged.
const unsigned kUnknownLogged = 8;
}  // namespace

bool SppService::knowsPhone() {
    pthread_mutex_lock(&mu_);
    bool known = !phoneAddress_.empty();
    pthread_mutex_unlock(&mu_);
    return known;
}

bool SppService::disconnectPhone(const std::string& device, const std::string& instance, const std::string& service) {
    pthread_mutex_lock(&mu_);
    std::string address = phoneAddress_;
    std::string named = service.empty() ? phoneService_ : service;
    pthread_mutex_unlock(&mu_);
    if (!gap_->attached() || address.empty()) return false;
    std::string d = device.empty() ? std::string(kDisconnectDevice) : device;
    std::string i = instance.empty() ? std::string(kDisconnectInstance) : instance;
    pthread_mutex_lock(&mu_);
    unknownLeft_ = kUnknownLogged;
    disconnectOut_ = true;
    pthread_mutex_unlock(&mu_);
    say("ServiceDisconnect device '%s', instance '%s', service '%s', for the phone of the last request for a link",
        d.c_str(), i.c_str(), named.c_str());
    if (send(kReqServiceDisconnect, buildServiceDisconnect(d, i, address, named))) return true;
    pthread_mutex_lock(&mu_);
    unknownLeft_ = 0;
    disconnectOut_ = false;
    pthread_mutex_unlock(&mu_);
    return false;
}

namespace {
// ServiceDisconnected informs are logged one by one up to this many; after that they are only counted.
const unsigned kEndsLogged = 8;
// unreadSaid_: which kind of message has been logged once as one that could not be read.
enum { kUnreadDeviceList = 1, kUnreadEnded = 2, kUnreadServiceList = 4, kUnreadVersion = 8, kUnreadDisconnect = 16 };
}  // namespace

// The replies and informs that are kept as state, or only logged: true if `id` is one of them. On the client's
// thread; nothing here sends or waits.
bool SppService::handleKnown(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len) {
    bool rejected = type == kTypeRejectedA || type == kTypeRejectedB;
    if (rejected) {
        if (id == kReqVersionGet) {
            pthread_mutex_lock(&mu_);
            versionAsked_ = false;
            pthread_mutex_unlock(&mu_);
            say("the Bluetooth server rejected VersionGet (type 0x%x)", type);
            return true;
        }
        if (id == kReqDeviceListGet) {
            pthread_mutex_lock(&mu_);
            bool refusal = refusalAsked_;
            refusalAsked_ = false;
            pthread_mutex_unlock(&mu_);
            say("the Bluetooth server rejected DeviceListGet (type 0x%x)", type);
            if (refusal) events_->sppRefusedPhone(PhoneInList());
            return true;
        }
        return false;
    }
    unsigned unread = 0;
    const char* what = 0;
    if (id == kInfDeviceListChanged || id == kRspDeviceListGet) {
        DeviceList l = parseDeviceList(id, payload, len);
        pthread_mutex_lock(&mu_);
        if (id == kInfDeviceListChanged) ++heard_.deviceListChanges;
        if (l.ok && l.error == 0) {
            heard_.devices = l.devices;
            heard_.devicesKnown = true;
        }
        bool refusal = id == kRspDeviceListGet && refusalAsked_;
        if (refusal) refusalAsked_ = false;
        std::string refused = refusedAddress_;
        pthread_mutex_unlock(&mu_);
        if (refusal) events_->sppRefusedPhone(phoneInList(l, refused));
        if (!l.ok) {
            unread = kUnreadDeviceList;
            what = "a device list";
        } else if (l.error != 0) {
            say("DeviceListGet reply: %s", gapErrorText(l.error, "").c_str());
        }
    } else if (id == kInfServiceDisconnected) {
        ServiceDisconnected d = parseServiceDisconnected(payload, len);
        pthread_mutex_lock(&mu_);
        ++heard_.linksEnded;
        unsigned n = d.ok ? ++endsLogged_ : 0;
        pthread_mutex_unlock(&mu_);
        if (!d.ok) {
            unread = kUnreadEnded;
            what = "ServiceDisconnected";
        } else if (n <= kEndsLogged) {
            char by[32];
            snprintf(by, sizeof(by), "reason %u", d.reason);
            say("the Bluetooth stack says a link ended: device '%s', instance '%s', service '%s', %s%s",
                shownText(d.device, 32).c_str(), shownText(d.instance, 32).c_str(), shownText(d.service, 40).c_str(),
                d.reason == 1 ? "ended by the phone" : (d.reason == 0 ? "ended by the car" : by),
                n == kEndsLogged ? ". Further ends of links are counted and not logged" : "");
        }
    } else if (id == kInfServiceListChanged) {
        ServiceListChanged c = parseServiceListChanged(payload, len);
        pthread_mutex_lock(&mu_);
        ++heard_.serviceListChanges;
        pthread_mutex_unlock(&mu_);
        if (!c.ok) {
            unread = kUnreadServiceList;
            what = "ServiceListChanged";
        }
    } else if (id == kRspVersionGet) {
        pthread_mutex_lock(&mu_);
        bool asked = versionAsked_;
        versionAsked_ = false;
        pthread_mutex_unlock(&mu_);
        if (!asked) return true;   // another client's
        StackVersion v = parseStackVersion(payload, len);
        if (!v.ok) {
            unread = kUnreadVersion;
            what = "VersionGet's reply";
        } else if (v.error != 0) {
            say("VersionGet reply: %s", gapErrorText(v.error, "").c_str());
        } else {
            std::string parts;
            for (size_t i = 0; i < v.parts.size(); ++i) parts += (i ? ", '" : "'") + shownText(v.parts[i], 64) + "'";
            say("the Bluetooth stack's version: %s", parts.c_str());
        }
    } else if (id == kRspServiceDisconnect) {
        pthread_mutex_lock(&mu_);
        bool out = disconnectOut_;
        disconnectOut_ = false;
        pthread_mutex_unlock(&mu_);
        if (!out) return true;     // another client's
        ServiceDisconnectReply d = parseServiceDisconnectReply(payload, len);
        if (!d.ok) {
            unread = kUnreadDisconnect;
            what = "ServiceDisconnect's reply";
        } else {
            say("ServiceDisconnect reply: %s, reason %u", gapErrorText(d.error, "").c_str(), d.reason);
        }
    } else if (id == kRspServiceConnect) {
        return true;               // this agent asks for none: another client's
    } else {
        return false;
    }
    if (unread != 0) {
        pthread_mutex_lock(&mu_);
        bool first = (unreadSaid_ & unread) == 0;
        unreadSaid_ |= unread;
        pthread_mutex_unlock(&mu_);
        if (first)
            say("%s from the Bluetooth server could not be read (id 0x%x, %u bytes); later ones that cannot be read "
                "are not logged", what, id, len);
    }
    return true;
}

// An answer this class does not know, after a ServiceDisconnect: its id, its type and its first 32-bit value, and
// never its strings, which may name the phone.
void SppService::noteUnknown(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len) {
    pthread_mutex_lock(&mu_);
    bool say1 = unknownLeft_ > 0;
    if (say1) --unknownLeft_;
    pthread_mutex_unlock(&mu_);
    if (!say1) return;
    if (len >= 4) {
        uint32_t first = readError(payload, len);
        say("after ServiceDisconnect, a message from the Bluetooth server that the agent does not know: id 0x%x, "
            "type 0x%x, first value %u (0x%x)", id, type, first, first);
    } else {
        say("after ServiceDisconnect, a message from the Bluetooth server that the agent does not know: id 0x%x, "
            "type 0x%x, %u bytes", id, type, len);
    }
}

bool SppService::send(uint32_t id, const std::vector<uint8_t>& payload) {
    if (gap_->request(id, payload)) return true;
    say("DSI request 0x%x not sent: %s", id,
        gap_->attached() ? "the send to the Bluetooth server failed" : "the Bluetooth server is not attached");
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
    pthread_mutex_lock(&mu_);
    clearError_ = kNoError;
    pthread_mutex_unlock(&mu_);
    set(kRegSent);
    sendRegister();
}

void SppService::gapDetached() {
    say("the Bluetooth server detached (its process stopped or restarted): the registration is gone with it");
    pthread_mutex_lock(&mu_);
    bool had = state_ == kRegSent || state_ == kClearSent || state_ == kRetrySent || state_ == kRegistered;
    if (had) state_ = kWaiting;
    subscribed_ = false;
    checkOut_ = false;
    // Every registration went with the server, the ones that removeOther() was asked to remove among them.
    otherWaiting_.clear();
    otherSent_.clear();
    // No reply comes from a server that has gone, and what it said of the phones is old.
    disconnectOut_ = false;
    refusalAsked_ = false;
    versionAsked_ = false;
    heard_.devicesKnown = false;
    heard_.devices.clear();
    pthread_mutex_unlock(&mu_);
    if (had) events_->sppState(kWaiting);
}

void SppService::gapMessage(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len) {
    if (isServiceRequestInform(id)) {
        // The phone's name and address are personal data: they stay here, and only the service goes on. The address
        // is kept, for a ServiceDisconnect, and never said.
        ServiceRequest q = parseServiceRequest(id, payload, len);
        if (id == kInfServiceRequest && q.ok && !q.address.empty()) {
            pthread_mutex_lock(&mu_);
            phoneAddress_ = q.address;
            phoneService_ = q.service;
            pthread_mutex_unlock(&mu_);
        }
        if (id == kInfServiceRequestError) {
            pthread_mutex_lock(&mu_);
            refusedAddress_ = q.ok ? q.address : std::string();
            pthread_mutex_unlock(&mu_);
        }
        events_->sppPhone(id == kInfServiceRequest ? kPhoneAsked
                                                   : (id == kInfServiceRequestComplete ? kPhoneLinked : kPhoneRefused),
                          q.service, q.ok);
        return;
    }
    bool rejected = type == kTypeRejectedA || type == kTypeRejectedB;
    if (rejected && id == kReqServiceDisconnect) {
        pthread_mutex_lock(&mu_);
        disconnectOut_ = false;
        pthread_mutex_unlock(&mu_);
        say("the Bluetooth server rejected ServiceDisconnect (type 0x%x)", type);
        return;
    }
    if (handleKnown(type, id, payload, len)) return;

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
            else say("SppUuidDeregister reply for '%s': %s", other.c_str(), deregisterErrorText(err).c_str());
            events_->sppOtherRemoved(other, err);
            sendNextOther();
            return;
        }
    }

    if ((id == kRspSppUuidRegister && !rejected) || (id == kReqSppUuidRegister && rejected)) {
        pthread_mutex_lock(&mu_);
        bool check = checkOut_;
        checkOut_ = false;
        pthread_mutex_unlock(&mu_);
        if (check) {
            checkAnswered(rejected ? kNoError : readError(payload, len), rejected ? type : 0);
            return;
        }
    }

    State s = state();
    if (id == kRspSppUuidRegister && !rejected) {
        uint32_t err = readError(payload, len);
        say("SppUuidRegister reply: %s", registerErrorText(err).c_str());
        // A reply that comes when no registration is waiting for one (after remove(), say) changes nothing.
        if (s != kRegSent && s != kRetrySent) return;
        if (err == 0) {
            set(kRegistered);
        } else if (s == kRegSent) {
            set(kClearSent);
            sendDeregister("clearing a registration an earlier run left behind, then trying again");
        } else {
            pthread_mutex_lock(&mu_);
            uint32_t cleared = clearError_;
            pthread_mutex_unlock(&mu_);
            // Nothing was filed under our name, and the stack still says DEFAULT.
            if (cleared == 10 && err == 5)
                say("nothing was registered under the name '%s', so its UUID %s is registered under another name, or "
                    "the car's Bluetooth is switched off", name_.c_str(), uuid_.c_str());
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
        say("SppUuidDeregister reply (id 0x%x): %s", id, deregisterErrorText(err).c_str());
        pthread_mutex_lock(&mu_);
        if (s == kClearSent) clearError_ = err;
        else removeError_ = err;
        pthread_mutex_unlock(&mu_);
        if (s == kClearSent) { set(kRetrySent); sendRegister(); }
        else set(err == 0 || err == 10 ? kRemoved : kRemoveFailed);
        sendNextOther();   // a removal of another program's registration may have waited for this answer
        return;
    }
    if (type == kTypeResponse && id != kRspSppUuidRegister && id != kRspSppUuidDeregister) noteUnknown(type, id, payload, len);
}

}  // namespace btspp
}  // namespace nbt

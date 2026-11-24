// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#include "spp_service.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "sha256.hpp"

namespace nbt {
namespace btspp {

namespace {

double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

// The lines said about Status and LocalName in the life of an SppService; after them, they are only counted.
const unsigned kAttributeLines = 60;

}  // namespace

std::string standInUuid(const std::string& name) {
    std::string seed = std::string(kStandInSeed) + name;
    uint8_t d[kSha256Bytes];
    sha256(seed.data(), seed.size(), d);
    d[6] = (uint8_t)((d[6] & 0x0f) | 0x80);   // version 8
    d[8] = (uint8_t)((d[8] & 0x3f) | 0x80);   // variant 10
    static const char kHex[] = "0123456789ABCDEF";
    std::string u;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) u += '-';
        u += kHex[d[i] >> 4];
        u += kHex[d[i] & 15];
    }
    return u;
}

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
    : gap_(gap), events_(events), type_(0), clock_(&monoNow), holdS_(0), statusWaitS_(kStatusWaitS), state_(kIdle),
      mode_(kModeOff), subscribed_(false), otherSentWhy_(kOtherLeftBehind), otherSentAt_(0),
      otherReplyS_(kOtherRemovalReplyS), bt_(kBtUnknown), statusHeard_(false), attachedAt_(0), openSince_(0),
      attributeLines_(0), regFor_(kForNone), regAt_(0), regStale_(false), regQuiet_(false), roundOn_(false),
      roundCheck_(false), retryDue_(false), yieldTried_(false), yielding_(false), yieldLeft_(0), checkFails_(0),
      timesRegistered_(0), rejectOut_(false), keptLooked_(false), keptWasOn_(false), askOut_(false),
      askStale_(false), askAt_(0), setOut_(false), setStale_(false), setOn_(false), setAt_(0), lookQuiet_(false),
      unknownLeft_(0), removeError_(kNoError), clearError_(kNoError),
      disconnectOut_(false), disconnectTold_(false), disconnectQuiet_(false), disconnectSentAt_(0), disconnectReplyS_(kDisconnectReplyS),
      connectOut_(false), lateKept_(false), pairingInforms_(false), refusalAsked_(false), versionAsked_(false), endsLogged_(0), unreadSaid_(0), checkOut_(false), checkSentAt_(0),
      unansweredSaid_(false), restoredRun_(0), oddRun_(false) {
    for (int i = 0; i < 2; ++i) {
        keptOdd_[i] = keptOddRejected_[i] = 0;
        keptOddKnown_[i] = false;
    }
    pthread_mutex_init(&mu_, NULL);
}

SppService::~SppService() { pthread_mutex_destroy(&mu_); }

void SppService::configure(const std::string& name, const std::string& uuid, uint32_t type) {
    name_ = name;
    uuid_ = uuid;
    type_ = type;
}

void SppService::say(const char* fmt, ...) {
    char line[1024];
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

unsigned SppService::timesRegistered() {
    pthread_mutex_lock(&mu_);
    unsigned n = timesRegistered_;
    pthread_mutex_unlock(&mu_);
    return n;
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
    // No registration of this object's goes from here: a round under way ends, and what waited for one with it.
    roundOn_ = false;
    roundLeft_.clear();
    retryDue_ = false;
    yielding_ = false;
    yielded_.clear();
    for (size_t i = 0; i < standIns_.size(); ++i) {
        if (standIns_[i].queued) standIns_[i].replacing = false;
        standIns_[i].queued = false;
    }
    pending_.clear();
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
    subscribe();   // once for each time the server attaches: nothing again if it was asked for already
    std::string line;
    pthread_mutex_lock(&mu_);
    bool waiting = state_ == kWaiting;
    bool begin = false;
    if (waiting) {
        state_ = kHeld;
        clearError_ = kNoError;
        begin = beginLocked(&line);
    }
    pthread_mutex_unlock(&mu_);
    if (!waiting) return;
    // kHeld is told only where the round cannot begin at once.
    if (begin) begun(line);
    else events_->sppState(kHeld);
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
    // The pairing informs that stop dials; not PairingComplete, which the agent has no use for.
    if (pairingInforms_) {
        subscribeTo(kInfPairingError);
        subscribeTo(kInfSimplePairingConfirmationRequired);
    }
    // Whether the car's Bluetooth is on, which the registrations wait for, and its name, which the car's own
    // software sets just before it makes its registrations: for the log.
    subscribeTo(kAttrStatus);
    subscribeTo(kAttrLocalName);
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
    double at = now();
    if (checkOut_ && at - checkSentAt_ >= replyS) {
        checkOut_ = false;
        lost = true;
        ++checks_.unanswered;
        sayLost = !unansweredSaid_;
        unansweredSaid_ = true;
        // Its round is given up, and so is a registration of that round's that is out: an answer that comes after
        // all is taken as the next one's, as the check's always was.
        if (roundOn_ && roundCheck_) {
            roundOn_ = false;
            roundLeft_.clear();
        }
        if (regFor_ == kForCheck || (regFor_ == kForStandIn && regQuiet_)) regFor_ = kForNone;
    }
    bool go = state_ == kRegistered && !checkOut_ && !roundOn_;
    unsigned fails = checkFails_;
    if (go) {
        checkOut_ = true;
        checkSentAt_ = at;
        ++checks_.sent;
        // The stand-ins go first, as in every round.
        roundOn_ = true;
        roundCheck_ = true;
        roundLeft_.clear();
        for (size_t i = 0; i < standIns_.size(); ++i)
            if (standIns_[i].wanted) roundLeft_.push_back(standIns_[i].name);
    }
    pthread_mutex_unlock(&mu_);
    if (lost && sayLost)
        say("the registration check of '%s' had no reply in %.0f s; until one is answered, later ones without a reply "
            "are only counted", name_.c_str(), replyS);
    if (!go) return false;
    pump();
    pthread_mutex_lock(&mu_);
    bool failed = checkFails_ != fails;
    pthread_mutex_unlock(&mu_);
    lookAtKept(true);   // and the kept service, quietly
    return !failed;
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
    queueOtherLocked(name, kOtherLeftBehind);
    pthread_mutex_unlock(&mu_);
    sendNextOther();
    return true;
}

// A name that is waiting or out already is not taken twice. mu_ held.
void SppService::queueOtherLocked(const std::string& name, OtherWhy why) {
    bool have = otherSent_ == name;
    for (size_t i = 0; i < otherWaiting_.size(); ++i) have = have || otherWaiting_[i].name == name;
    if (!have) otherWaiting_.push_back(Other(name, why));
}

namespace {
const char* otherWhyText(int why) {
    switch (why) {
    case 1: return "what is registered under it, whose program is not running: the agent's stand-in takes its place";
    case 2: return "the agent's stand-in, given up so that the agent's own serial service has a place";
    case 3: return "the agent's stand-in, as Bluetooth serial stops, and its program is not running";
    case 4: return "the agent's stand-in for a service that is no longer declared";
    default: return "another program's registration, left behind";
    }
}
}  // namespace

// Sends the next removal that waits, unless one is out already or a removal of our own waits for its reply: the
// replies carry no name, and are told apart only by the order they come in.
void SppService::sendNextOther() {
    std::string lost, next;
    OtherWhy lostWhy = kOtherLeftBehind, nextWhy = kOtherLeftBehind;
    pthread_mutex_lock(&mu_);
    double at = now();
    if (!otherSent_.empty() && at - otherSentAt_ > otherReplyS_) {
        lost = otherSent_;
        lostWhy = otherSentWhy_;
        otherSent_.clear();
    }
    bool ownOut = state_ == kClearSent || state_ == kRemoveSent;
    if (otherSent_.empty() && !ownOut && !otherWaiting_.empty()) {
        next = otherWaiting_.front().name;
        nextWhy = otherWaiting_.front().why;
        otherWaiting_.erase(otherWaiting_.begin());
        otherSent_ = next;
        otherSentWhy_ = nextWhy;
        otherSentAt_ = at;
    }
    pthread_mutex_unlock(&mu_);
    if (!lost.empty()) {
        say("no reply to the removal of '%s' in %.0f s", lost.c_str(), otherReplyS_);
        events_->sppOtherRemoved(lost, kNoError);
        otherAnswered(Other(lost, lostWhy), kNoError);
    }
    if (next.empty()) return;
    say("SppUuidDeregister name '%s': %s", next.c_str(), otherWhyText(nextWhy));
    if (send(kReqSppUuidDeregister, buildDeregister(next))) return;
    pthread_mutex_lock(&mu_);
    if (otherSent_ == next) otherSent_.clear();
    pthread_mutex_unlock(&mu_);
    events_->sppOtherRemoved(next, kNoError);
    otherAnswered(Other(next, nextWhy), kNoError);
}

// ---- the stand-ins, and when registrations may go ----------------------------------------------------------------

SppService::StandIn* SppService::standInLocked(const std::string& name) {
    for (size_t i = 0; i < standIns_.size(); ++i)
        if (standIns_[i].name == name) return &standIns_[i];
    return 0;
}

bool SppService::mayRegisterLocked(double at) const {
    if (bt_ == kBtOn) return at - openSince_ >= holdS_;
    if (bt_ == kBtOff) return false;
    return at - attachedAt_ >= statusWaitS_;
}

bool SppService::attributeLineLocked(bool* last) {
    *last = false;
    if (attributeLines_ >= kAttributeLines) return false;
    *last = ++attributeLines_ == kAttributeLines;
    return true;
}

void SppService::setStandIns(const std::vector<std::string>& names) {
    bool drops = false;
    pthread_mutex_lock(&mu_);
    std::vector<StandIn> kept;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        if (!isServiceName(n) || n == name_) continue;
        bool twice = false;
        for (size_t k = 0; k < kept.size(); ++k) twice = twice || kept[k].name == n;
        if (twice) continue;
        StandIn* had = standInLocked(n);
        StandIn s;
        if (had != 0) {
            s = *had;
        } else {
            s.name = n;
            s.uuid = standInUuid(n);
        }
        s.wanted = true;
        kept.push_back(s);
    }
    for (size_t i = 0; i < standIns_.size(); ++i) {
        StandIn s = standIns_[i];
        bool given = false;
        for (size_t k = 0; k < kept.size(); ++k) given = given || kept[k].name == s.name;
        if (given) continue;
        if (s.ours) {
            s.ours = false;
            queueOtherLocked(s.name, kOtherDropped);
            drops = true;
        }
        for (size_t k = 0; k < pending_.size(); ++k) {
            if (pending_[k] != s.name) continue;
            pending_.erase(pending_.begin() + k);
            break;
        }
        // Kept until its registration that is out is answered: an answer 0 then has it removed (standInAnswered()).
        if (regFor_ == kForStandIn && regName_ == s.name) {
            s.wanted = false;
            s.queued = false;
            kept.push_back(s);
        }
    }
    standIns_ = kept;
    pthread_mutex_unlock(&mu_);
    if (drops) sendNextOther();
}

void SppService::standInRuns(const std::string& name) {
    pthread_mutex_lock(&mu_);
    StandIn* s = standInLocked(name);
    if (s != 0) s->ours = false;
    pthread_mutex_unlock(&mu_);
}

SppService::Replace SppService::replaceWithStandIn(const std::string& name) {
    if (name == name_ || !isServiceName(name) || !gap_->attached()) return kNotNow;
    pthread_mutex_lock(&mu_);
    if (mode_ != kModeKeep || state_ == kHeld || state_ == kWaiting || !mayRegisterLocked(now())) {
        pthread_mutex_unlock(&mu_);
        return kNotNow;
    }
    StandIn* s = standInLocked(name);
    if (s == 0) {
        StandIn fresh;
        fresh.name = name;
        fresh.uuid = standInUuid(name);
        fresh.wanted = true;
        standIns_.push_back(fresh);
        s = &standIns_.back();
    }
    Replace r = kKeptOurs;
    if (!s->ours) {
        r = kReplacing;
        if (!s->replacing) {
            s->replacing = true;
            queueOtherLocked(name, kOtherReplace);
        }
    }
    pthread_mutex_unlock(&mu_);
    if (r == kReplacing) sendNextOther();
    return r;
}

unsigned SppService::removeStandIns() {
    if (!gap_->attached()) return 0;   // they went with the server
    unsigned n = 0;
    pthread_mutex_lock(&mu_);
    for (size_t i = 0; i < standIns_.size(); ++i) {
        if (!standIns_[i].ours) continue;
        standIns_[i].ours = false;
        queueOtherLocked(standIns_[i].name, kOtherStop);
        ++n;
    }
    pthread_mutex_unlock(&mu_);
    if (n > 0) sendNextOther();
    return n;
}

bool SppService::othersPending() {
    sendNextOther();   // one with no reply in its time is given up here, and the next sent
    pthread_mutex_lock(&mu_);
    bool pending = !otherWaiting_.empty() || !otherSent_.empty();
    pthread_mutex_unlock(&mu_);
    return pending && gap_->attached();
}

std::string SppService::namesText(const std::vector<std::string>& names) {
    std::string t;
    for (size_t i = 0; i < names.size(); ++i) t += (i ? ", '" : "'") + names[i] + "'";
    return t;
}

std::vector<SppService::StandInState> SppService::standIns() {
    std::vector<StandInState> out;
    pthread_mutex_lock(&mu_);
    for (size_t i = 0; i < standIns_.size(); ++i) {
        const StandIn& s = standIns_[i];
        if (!s.wanted) continue;
        StandInState t;
        t.name = s.name;
        t.uuid = s.uuid;
        t.ours = s.ours;
        t.sent = s.sent;
        t.placed = s.placed;
        t.there = s.there;
        t.other = s.other;
        out.push_back(t);
    }
    pthread_mutex_unlock(&mu_);
    return out;
}

SppService::Bluetooth SppService::bluetooth() {
    pthread_mutex_lock(&mu_);
    Bluetooth b = bt_;
    pthread_mutex_unlock(&mu_);
    return b;
}

void SppService::tend() {
    bool stale = false;
    pthread_mutex_lock(&mu_);
    if (regFor_ != kForNone && regStale_ && now() - regAt_ >= kStaleReplyS) {
        regFor_ = kForNone;
        regStale_ = false;
        stale = true;
    }
    pthread_mutex_unlock(&mu_);
    if (stale) {
        say("no answer came in %.0f s to a serial registration that went out before the car's Bluetooth was switched "
            "off or the server went: it is passed over, and the next goes", kStaleReplyS);
        pump();
    }
    // Only a round that begins here is sent from here: what else is due goes on from the answers, on the thread they
    // come on.
    advance();
}

bool SppService::beginLocked(std::string* line) {
    double at = now();
    if (state_ != kHeld || mode_ != kModeKeep || !mayRegisterLocked(at)) return false;
    state_ = kRegSent;
    clearError_ = kNoError;
    yieldTried_ = false;
    roundOn_ = true;
    roundCheck_ = false;
    roundLeft_.clear();
    for (size_t i = 0; i < standIns_.size(); ++i)
        if (standIns_[i].wanted) roundLeft_.push_back(standIns_[i].name);
    unsigned n = (unsigned)roundLeft_.size();
    double since = at - (bt_ == kBtOn ? openSince_ : attachedAt_);
    char what[160];
    if (n == 0)
        snprintf(what, sizeof what, "the agent's own; no serial service of another program is declared");
    else
        snprintf(what, sizeof what, "the stand-in%s for the %u declared serial service%s of other programs first, then "
                 "the agent's own", n == 1 ? "" : "s", n, n == 1 ? "" : "s");
    char text[400];
    if (bt_ == kBtOn)
        snprintf(text, sizeof text, "the serial registrations go out now, %.1f s after the car's Bluetooth was seen on: "
                 "%s", since, what);
    else
        snprintf(text, sizeof text, "the car's Bluetooth stack has said nothing of whether it is on (Status) in %.1f s "
                 "since the attach: the serial registrations go out now all the same, as with no hold: %s", since, what);
    *line = text;
    return true;
}

void SppService::begun(const std::string& line) {
    say("%s", line.c_str());
    events_->sppState(kRegSent);
    pump();
}

bool SppService::advance() {
    if (!gap_->attached()) return false;
    std::string line;
    pthread_mutex_lock(&mu_);
    bool begin = beginLocked(&line);
    pthread_mutex_unlock(&mu_);
    if (!begin) return false;
    begun(line);
    return true;
}

void SppService::pump() {
    for (;;) {
        if (!gap_->attached()) return;
        RegFor what = kForNone;
        std::string name, uuid;
        bool quiet = false;
        pthread_mutex_lock(&mu_);
        double at = now();
        if (regFor_ != kForNone || mode_ != kModeKeep || !mayRegisterLocked(at)) {
            pthread_mutex_unlock(&mu_);
            return;
        }
        if (retryDue_) {
            retryDue_ = false;
            if (state_ == kRetrySent) what = kForOwn;
        } else if (roundOn_) {
            while (what == kForNone && !roundLeft_.empty()) {
                StandIn* s = standInLocked(roundLeft_.front());
                roundLeft_.erase(roundLeft_.begin());
                if (s == 0 || !s->wanted) continue;
                what = kForStandIn;
                name = s->name;
                uuid = s->uuid;
                ++s->sent;
            }
            quiet = roundCheck_;
            if (what == kForNone) {
                roundOn_ = false;
                if (roundCheck_ && checkOut_ && state_ == kRegistered) what = kForCheck;
                else if (!roundCheck_ && state_ == kRegSent) what = kForOwn;
                else if (roundCheck_ && checkOut_) {
                    // the registration is not there any more to be checked: the check is not made
                    checkOut_ = false;
                    --checks_.sent;
                }
            }
        } else if (!pending_.empty()) {
            StandIn* s = standInLocked(pending_.front());
            pending_.erase(pending_.begin());
            if (s != 0) s->queued = false;
            if (s != 0 && s->wanted) {
                what = kForStandIn;
                name = s->name;
                uuid = s->uuid;
                ++s->sent;
            } else if (s != 0) {
                s->replacing = false;
            }
        }
        if (what == kForNone) {
            bool more = retryDue_ || roundOn_ || !pending_.empty();
            pthread_mutex_unlock(&mu_);
            if (more) continue;
            return;
        }
        regFor_ = what;
        regName_ = name;
        regAt_ = at;
        regStale_ = false;
        regQuiet_ = quiet;
        pthread_mutex_unlock(&mu_);

        bool sent;
        if (what == kForCheck) {
            // Quietly: the check is not a registration of its own, and goes out every half minute.
            sent = gap_->request(kReqSppUuidRegister, buildRegister(name_, uuid_, uuid_, type_));
        } else if (what == kForOwn) {
            say("SppUuidRegister name '%s', UUID %s, type %u", name_.c_str(), uuid_.c_str(), type_);
            sent = send(kReqSppUuidRegister, buildRegister(name_, uuid_, uuid_, type_));
        } else if (quiet) {
            sent = gap_->request(kReqSppUuidRegister, buildRegister(name, uuid, uuid, type_));
        } else {
            say("SppUuidRegister name '%s', UUID %s, type %u: the stand-in for another program's serial service, ahead "
                "of the agent's own", name.c_str(), uuid.c_str(), type_);
            sent = send(kReqSppUuidRegister, buildRegister(name, uuid, uuid, type_));
        }
        if (sent) return;   // what follows goes on from its answer
        pthread_mutex_lock(&mu_);
        if (regFor_ == what && regName_ == name) regFor_ = kForNone;
        if (what == kForCheck) {
            checkOut_ = false;
            --checks_.sent;
            ++checkFails_;
        } else if (what == kForStandIn) {
            StandIn* s = standInLocked(name);
            if (s != 0) s->replacing = false;
        }
        pthread_mutex_unlock(&mu_);
        // This object's own that could not be sent waits, as it always has, for the caller's time limit.
        if (what != kForStandIn) return;
    }
}

void SppService::registerAnswered(uint32_t err, uint32_t rejectedType) {
    pthread_mutex_lock(&mu_);
    RegFor what = regFor_;
    std::string name = regName_;
    bool stale = regStale_, quiet = regQuiet_;
    regFor_ = kForNone;
    regStale_ = false;
    if (what == kForCheck && !stale) checkOut_ = false;
    pthread_mutex_unlock(&mu_);
    if (stale) {
        char rejection[48];
        snprintf(rejection, sizeof rejection, "a rejection (type 0x%x)", rejectedType);
        say("the answer to a serial registration that went out before the car's Bluetooth was switched off or the "
            "server went (%s): %s, passed over",
            what == kForStandIn ? ("the stand-in '" + name + "'").c_str() : (what == kForCheck ? "a check" : "the agent's own"),
            rejectedType != 0 ? rejection : registerErrorText(err).c_str());
    } else if (what == kForCheck) {
        checkAnswered(err, rejectedType);
    } else if (what == kForStandIn) {
        standInAnswered(name, err, rejectedType, quiet);
    } else {
        ownAnswered(err, rejectedType, what == kForOwn);
    }
    pump();
}

void SppService::ownAnswered(uint32_t err, uint32_t rejectedType, bool waited) {
    State s = state();
    bool ownTry = waited && (s == kRegSent || s == kRetrySent);
    if (rejectedType != 0) {
        say("the Bluetooth server rejected the request (type 0x%x, id 0x%x)", rejectedType, kReqSppUuidRegister);
        if (ownTry) {
            pthread_mutex_lock(&mu_);
            yielding_ = false;
            yielded_.clear();
            pthread_mutex_unlock(&mu_);
            set(kFailed);
        }
        sendNextOther();
        return;
    }
    say("SppUuidRegister reply: %s", registerErrorText(err).c_str());
    // A reply that comes when no registration of ours waits for one (after remove(), say) changes nothing.
    if (!ownTry) return;
    if (err == 0) {
        pthread_mutex_lock(&mu_);
        std::string given = namesText(yielded_);
        std::vector<std::string> kept;
        // The stand-ins given up are not registered again in this round: they stay given up, and go first in the next.
        for (size_t i = 0; i < standIns_.size(); ++i)
            if (standIns_[i].ours && standIns_[i].wanted) kept.push_back(standIns_[i].name);
        size_t yielded = yielded_.size();
        yielding_ = false;
        yielded_.clear();
        ++timesRegistered_;
        pthread_mutex_unlock(&mu_);
        if (yielded > 0)
            say("the agent's own serial service '%s' has a place now, the one the stand-in%s given up (%s) left; "
                "stand-ins that still hold their names: %s", name_.c_str(), yielded == 1 ? "" : "s", given.c_str(),
                kept.empty() ? "none" : namesText(kept).c_str());
        set(kRegistered);
        lookAtKept(false);   // after the agent's own registration: the kept service
        return;
    }
    if (s == kRegSent) {
        set(kClearSent);
        sendDeregister("clearing a registration an earlier run left behind, then trying again");
        return;
    }
    // The second registration, or one after a stand-in was given up, is refused too. No place left, in the header:
    // the last stand-in that is ours, in the order of the round, is given up, and the agent's own goes once more.
    std::string last;
    pthread_mutex_lock(&mu_);
    uint32_t cleared = clearError_;
    bool gaveUp = yieldTried_;
    yielding_ = false;
    if (err == 5) {
        for (size_t i = standIns_.size(); i-- > 0 && last.empty();)
            if (standIns_[i].ours && standIns_[i].wanted) last = standIns_[i].name;
    }
    if (!last.empty()) {
        standInLocked(last)->ours = false;
        yieldTried_ = true;
        yielding_ = true;
        yieldLeft_ = 1;
        yielded_.push_back(last);
        queueOtherLocked(last, kOtherYield);
    } else {
        yielded_.clear();
    }
    pthread_mutex_unlock(&mu_);
    if (!last.empty()) {
        say("the car's Bluetooth service did not take the agent's own serial service '%s' (%s) with stand-ins of the "
            "agent's registered ahead of it: the stack may have no place left, and the agent's own service must have "
            "one. So the last of them, '%s', is given up: it is removed, and the agent's own registered once more in its "
            "place. The declared service '%s' has no stand-in for now: its program may be refused a place, or get one "
            "that no phone finds", name_.c_str(), registerErrorText(err).c_str(), last.c_str(), last.c_str());
        sendNextOther();
        return;
    }
    // Nothing was filed under our name, and the stack still says DEFAULT.
    if (cleared == 10 && err == 5)
        say("nothing was registered under the name '%s', so its UUID %s is registered under another name, the stack "
            "has no place left for it, or the car's Bluetooth is switched off", name_.c_str(), uuid_.c_str());
    if (gaveUp)
        say("the agent's own serial service '%s' was refused after the stand-ins were given up too (%s)", name_.c_str(),
            registerErrorText(err).c_str());
    set(kFailed);
}

void SppService::standInAnswered(const std::string& name, uint32_t err, uint32_t rejectedType, bool quiet) {
    bool news = true, drop = false;
    pthread_mutex_lock(&mu_);
    StandIn* s = standInLocked(name);
    if (s != 0) {
        news = !quiet || !s->lastKnown || s->last != err || s->lastRejected != rejectedType;
        s->last = err;
        s->lastRejected = rejectedType;
        s->lastKnown = true;
        s->replacing = false;
        if (rejectedType == 0 && err == 0) {
            s->ours = true;
            ++s->placed;
        } else if (rejectedType == 0 && err == 5) {
            ++s->there;
        } else {
            ++s->other;
        }
        if (!s->wanted) {
            if (s->ours) {
                queueOtherLocked(name, kOtherDropped);
                drop = true;
            }
            for (size_t i = 0; i < standIns_.size(); ++i) {
                if (standIns_[i].name != name) continue;
                standIns_.erase(standIns_.begin() + i);
                break;
            }
        }
    }
    pthread_mutex_unlock(&mu_);
    if (news) {
        const char* in = quiet ? ", in a registration check's round" : "";
        if (rejectedType != 0)
            say("the Bluetooth server rejected the stand-in '%s' (type 0x%x)%s: it is not known to be there",
                name.c_str(), rejectedType, in);
        else if (err == 0)
            say("SppUuidRegister reply for the stand-in '%s'%s: 0 OK: it holds the name, and its place, for the service's "
                "program", name.c_str(), in);
        else if (err == 5)
            say("SppUuidRegister reply for the stand-in '%s'%s: %s: it is left as it is", name.c_str(), in,
                registerErrorText(err).c_str());
        else
            say("SppUuidRegister reply for the stand-in '%s'%s: %s: it is not known to be there", name.c_str(), in,
                registerErrorText(err).c_str());
    }
    if (drop) sendNextOther();
}

void SppService::otherAnswered(const Other& other, uint32_t err) {
    bool go = false, left = false, odd = false;
    pthread_mutex_lock(&mu_);
    if (other.why == kOtherReplace) {
        StandIn* s = standInLocked(other.name);
        if (s != 0 && s->replacing && !s->queued) {
            // 10: nothing was there; 0: it is gone. Either way the stand-in goes in its place at once.
            if ((err == 0 || err == 10) && s->wanted && mode_ == kModeKeep) {
                s->queued = true;
                pending_.push_back(other.name);
                go = true;
            } else {
                s->replacing = false;
                left = err != kNoError;
            }
        }
    } else if (other.why == kOtherYield) {
        // Whatever the answer, the agent's own goes once more. A stand-in whose removal was refused may hold its place
        // still: then the agent's own is refused again and the next stand-in given up, so this ends.
        if (yielding_ && yieldLeft_ > 0 && --yieldLeft_ == 0) {
            retryDue_ = true;
            go = true;
            odd = err != 0 && err != 10;
        }
    }
    pthread_mutex_unlock(&mu_);
    if (odd)
        say("the removal of the stand-in '%s' was answered %s: it may hold its place still; the agent's own serial "
            "service is registered once more all the same", other.name.c_str(),
            err == kNoError ? "with nothing (no reply in time, or a rejection)" : deregisterErrorText(err).c_str());
    if (left)
        say("what is registered under '%s' was not removed (%s): no stand-in is put in its place", other.name.c_str(),
            deregisterErrorText(err).c_str());
    if (go) pump();
}

// ---- a request refused, and a service of the car's kept on --------------------------------------------------------

bool SppService::rejectRequest(const std::string& address, const std::string& service) {
    if (!gap_->attached()) return false;
    pthread_mutex_lock(&mu_);
    bool busy = rejectOut_;
    rejectOut_ = true;
    pthread_mutex_unlock(&mu_);
    if (busy) return false;
    if (gap_->request(kReqServiceReject, buildServiceReject(address, service))) return true;
    pthread_mutex_lock(&mu_);
    rejectOut_ = false;
    pthread_mutex_unlock(&mu_);
    return false;
}

SppService::KeptState SppService::kept() {
    pthread_mutex_lock(&mu_);
    KeptState k = kept_;
    pthread_mutex_unlock(&mu_);
    return k;
}

bool SppService::keptSetOut() {
    pthread_mutex_lock(&mu_);
    bool out = setOut_;
    pthread_mutex_unlock(&mu_);
    return out && gap_->attached();
}

// Whether an answer to the look (0) or the switch (1) that says nothing of the service is news: not the same as the
// last such one of that kind with a good one of that kind in between. mu_ not held.
bool SppService::keptOddNews(int which, uint32_t err, uint32_t rejectedType) {
    pthread_mutex_lock(&mu_);
    bool news = !keptOddKnown_[which] || keptOdd_[which] != err || keptOddRejected_[which] != rejectedType;
    keptOddKnown_[which] = true;
    keptOdd_[which] = err;
    keptOddRejected_[which] = rejectedType;
    pthread_mutex_unlock(&mu_);
    return news;
}

void SppService::lookAtKept(bool quiet) {
    if (keptName_.empty() || !gap_->attached()) return;
    bool lost = false;
    pthread_mutex_lock(&mu_);
    double at = now();
    // One that has had no answer in all this time is given up, so that the next may go.
    if (askOut_ && at - askAt_ >= kStaleReplyS) {
        askOut_ = askStale_ = false;
        lost = true;
    }
    if (setOut_ && at - setAt_ >= kStaleReplyS) setOut_ = setStale_ = false;
    bool go = !askOut_ && !setOut_ && mode_ == kModeKeep && mayRegisterLocked(at);
    if (go) {
        askOut_ = true;
        askStale_ = false;
        askAt_ = at;
        lookQuiet_ = quiet;
    }
    pthread_mutex_unlock(&mu_);
    if (lost && keptOddNews(0, kNoError, 1))
        say("SupportedServicesGet had no answer in %.0f s: whether the car's service '%s' is on is not known", kStaleReplyS,
            keptName_.c_str());
    if (!go) return;
    if (gap_->request(kReqSupportedServicesGet, std::vector<uint8_t>())) return;
    pthread_mutex_lock(&mu_);
    askOut_ = false;
    pthread_mutex_unlock(&mu_);
    if (!quiet) say("SupportedServicesGet not sent: %s", gap_->attached() ? "the send failed" : "the server is not attached");
}

void SppService::sendKeptSet(bool on) {
    pthread_mutex_lock(&mu_);
    bool busy = setOut_;
    if (!busy) {
        setOut_ = true;
        setStale_ = false;
        setOn_ = on;
        setAt_ = now();
    }
    pthread_mutex_unlock(&mu_);
    if (busy) return;
    if (gap_->request(kReqEnabledServicesSet, buildEnabledServicesSet(keptName_, on))) return;
    pthread_mutex_lock(&mu_);
    setOut_ = false;
    pthread_mutex_unlock(&mu_);
    say("EnabledServicesSet for the car's service '%s' not sent: %s; tried again at the next registration check",
        keptName_.c_str(), gap_->attached() ? "the send failed" : "the server is not attached");
}

bool SppService::switchKeptOff() {
    if (keptName_.empty() || !gap_->attached()) return false;
    pthread_mutex_lock(&mu_);
    bool go = kept_.foundOffFirst && kept_.switchedOn && !setOut_;
    pthread_mutex_unlock(&mu_);
    if (!go) return false;
    sendKeptSet(false);
    return keptSetOut();
}

void SppService::keptAnswered(uint32_t id, uint32_t rejectedType, const uint8_t* payload, uint32_t len) {
    bool ask = rejectedType != 0 ? id == kReqSupportedServicesGet : id == kRspSupportedServicesGet;
    const char* name = keptName_.c_str();
    pthread_mutex_lock(&mu_);
    bool out = ask ? askOut_ : setOut_;
    bool stale = ask ? askStale_ : setStale_;
    bool quiet = lookQuiet_, on = setOn_;
    if (ask) askOut_ = askStale_ = false;
    else setOut_ = setStale_ = false;
    pthread_mutex_unlock(&mu_);
    if (!out) return;   // none of ours: nothing
    const char* what = ask ? "SupportedServicesGet" : "EnabledServicesSet";
    if (stale) {
        say("the answer to %s that went out before the car's Bluetooth was switched off or the server went: passed over",
            what);
        return;
    }
    if (!ask) {
        uint32_t err = rejectedType != 0 ? kNoError : readError(payload, len);
        if (rejectedType != 0 || err != 0) {
            char answer[64];
            snprintf(answer, sizeof answer, "a rejection (type 0x%x)", rejectedType);
            if (keptOddNews(1, err, rejectedType))
                say("EnabledServicesSet switching the car's service '%s' %s was answered %s: tried again at the next "
                    "registration check", name, on ? "on" : "off",
                    rejectedType != 0 ? answer : gapErrorText(err, "").c_str());
            return;
        }
        unsigned switches = 0;
        pthread_mutex_lock(&mu_);
        keptOddKnown_[1] = false;
        if (on) {
            kept_.state = kKeptOn;
            kept_.switchedOn = true;
            switches = ++kept_.switches;
            keptWasOn_ = true;
        } else {
            kept_.state = kKeptOff;
            kept_.switchedOn = false;
            keptWasOn_ = false;
        }
        pthread_mutex_unlock(&mu_);
        if (!on) say("the car's service '%s' is switched off again, as it was found (EnabledServicesSet answered 0 OK)", name);
        else if (switches == 1) say("the car's service '%s' is switched on (EnabledServicesSet answered 0 OK)", name);
        return;
    }
    SupportedServices s;
    if (rejectedType == 0) s = parseSupportedServices(payload, len);
    if (rejectedType != 0 || !s.ok || s.error != 0) {
        uint32_t err = rejectedType != 0 || !s.ok ? kNoError : s.error;
        char answer[64];
        snprintf(answer, sizeof answer, "a rejection (type 0x%x)", rejectedType);
        if (keptOddNews(0, err, rejectedType != 0 ? rejectedType : (s.ok ? 0u : 2u)))
            say("SupportedServicesGet was answered %s: whether the car's service '%s' is on is not known; asked again at "
                "the next registration check", rejectedType != 0 ? answer : (!s.ok ? "with a reply that could not be read"
                                                                                  : gapErrorText(err, "").c_str()),
                name);
        return;
    }
    int state = s.state(keptName_);
    bool first = false, sayAbsent = false, switchOn = false, offAgain = false;
    unsigned again = 0;
    pthread_mutex_lock(&mu_);
    keptOddKnown_[0] = false;
    first = !keptLooked_;
    keptLooked_ = true;
    if (state < 0) {
        kept_.state = kKeptAbsent;
        sayAbsent = !kept_.absentSeen;
        kept_.absentSeen = true;
    } else if (state == 1) {
        kept_.state = kKeptOn;
        keptWasOn_ = true;
    } else {
        kept_.state = kKeptOff;
        if (first) kept_.foundOffFirst = true;
        offAgain = keptWasOn_;
        if (offAgain) again = ++kept_.offAgain;
        keptWasOn_ = false;
        switchOn = mode_ == kModeKeep;
    }
    pthread_mutex_unlock(&mu_);
    if (sayAbsent)
        say("the car's Bluetooth stack lists no service '%s' among its %u (SupportedServicesGet): there is nothing to "
            "switch on", name, (unsigned)s.names.size());
    else if (state == 1 && first)
        say("the car's service '%s' is on (SupportedServicesGet): it is left as it is", name);
    else if (state == 0 && first)
        say("the car's service '%s' is off (SupportedServicesGet): it is switched on (EnabledServicesSet), and kept on "
            "while the agent runs", name);
    else if (state == 0 && offAgain && again == 1)
        say("the car's service '%s' has been switched off since it was last seen on, as the car's own software does on "
            "some cars with no word to other clients: it is switched on again. From now on this is only counted", name);
    else if (state == 0 && !offAgain && !quiet)
        say("the car's service '%s' is off (SupportedServicesGet): it is switched on again", name);
    if (switchOn) sendKeptSet(true);
}

// ---- what the server says of the car's Bluetooth ------------------------------------------------------------------

void SppService::forgetRegistrationsLocked() {
    for (size_t i = 0; i < standIns_.size(); ++i) {
        standIns_[i].ours = false;
        standIns_[i].replacing = false;
        standIns_[i].queued = false;
    }
    for (size_t i = standIns_.size(); i-- > 0;)
        if (!standIns_[i].wanted) standIns_.erase(standIns_.begin() + i);
    pending_.clear();
    roundOn_ = false;
    roundLeft_.clear();
    retryDue_ = false;
    yielding_ = false;
    yieldLeft_ = 0;
    yielded_.clear();
    checkOut_ = false;
    // The registrations that removeOther() was asked to remove went too.
    otherWaiting_.clear();
    if (regFor_ != kForNone) regStale_ = true;
    // Nothing is known of the kept service until the next look; an answer to a request of it that is out is passed
    // over. Whether it was seen on stays: found off at the next look, it was switched off meanwhile.
    if (askOut_) askStale_ = true;
    if (setOut_) setStale_ = true;
    kept_.state = kKeptNotLooked;
}

void SppService::statusTold(const uint8_t* payload, uint32_t len) {
    Reader r(payload, len);
    uint32_t value = r.u32();
    if (!r.ok()) {
        pthread_mutex_lock(&mu_);
        bool first = (unreadSaid_ & 128) == 0;
        unreadSaid_ |= 128;
        pthread_mutex_unlock(&mu_);
        if (first)
            say("Status from the Bluetooth server could not be read (%u bytes); later ones that cannot be read are not "
                "logged", len);
        return;
    }
    Bluetooth is = value == 1 ? kBtOn : (value == 0 ? kBtOff : kBtUnknown);
    bool closedNow = false, held = false, sayIt = false, last = false;
    unsigned ours = 0;
    pthread_mutex_lock(&mu_);
    double at = now();
    bool first = !statusHeard_;
    statusHeard_ = true;
    Bluetooth was = bt_;
    bool changed = first || is != was;
    if (changed) {
        ++heard_.statusChanges;
        sayIt = attributeLineLocked(&last);
    }
    if (is == kBtOn && (was != kBtOn || first)) openSince_ = first ? attachedAt_ : at;
    if (is == kBtOff && was != kBtOff) {
        closedNow = !first;
        for (size_t i = 0; i < standIns_.size(); ++i) ours += standIns_[i].ours ? 1 : 0;
        bool had = state_ == kRegSent || state_ == kClearSent || state_ == kRetrySent || state_ == kRegistered;
        if (mode_ == kModeKeep && had) {
            state_ = kHeld;
            held = true;
        }
        forgetRegistrationsLocked();
    }
    bt_ = is;
    double hold = holdS_;
    pthread_mutex_unlock(&mu_);
    if (sayIt) {
        const char* more = last ? ". Further changes of the car's Bluetooth state and name are counted and not logged" : "";
        char ahead[96] = "";
        if (ours > 0) snprintf(ahead, sizeof ahead, ", and %u stand-in%s of the agent's", ours, ours == 1 ? "" : "s");
        if (is == kBtUnknown)
            say("the Bluetooth server gave Status %u, which is neither on (1) nor off (0): taken as not known, so the "
                "serial registrations go %.0f s after the attach at the latest%s", value, statusWaitS_, more);
        else if (is == kBtOn && hold > 0)
            say("the car's Bluetooth %s (Status 1): no serial registration goes out for %.0f s%s, so that the car's own "
                "are made first%s", first ? "is on as the agent attaches" : "has been switched on", hold,
                first ? ", counted from the attach" : "", more);
        else if (is == kBtOn)
            say("the car's Bluetooth %s (Status 1): the serial registrations may go out at once (no hold)%s",
                first ? "is on as the agent attaches" : "has been switched on", more);
        else if (!closedNow)
            say("the car's Bluetooth is off (Status 0) as the agent attaches: no serial registration goes out until it is "
                "on and has been for %.0f s%s", hold, more);
        else
            say("the car's Bluetooth has been switched off (Status 0): every serial registration went with it, the "
                "agent's own%s; no registration and no registration check goes out until it is on again and has been "
                "for %.0f s%s", ahead, hold, more);
    }
    if (held) events_->sppState(kHeld);
    if (is != kBtOff) advance();
}

void SppService::nameTold(const uint8_t* payload, uint32_t len) {
    Reader r(payload, len);
    std::string name = r.str();
    bool sayIt = false, last = false;
    pthread_mutex_lock(&mu_);
    if (r.ok()) {
        ++heard_.names;
        sayIt = attributeLineLocked(&last);
    }
    bool first = !r.ok() && (unreadSaid_ & 256) == 0;
    if (!r.ok()) unreadSaid_ |= 256;
    pthread_mutex_unlock(&mu_);
    if (first)
        say("LocalName from the Bluetooth server could not be read (%u bytes); later ones that cannot be read are not "
            "logged", len);
    if (!sayIt) return;
    // The name may be a person's: only whether it is there, and its length.
    const char* more = last ? ". Further changes of the car's Bluetooth state and name are counted and not logged" : "";
    if (name.empty())
        say("the car's Bluetooth name is cleared (LocalName ''), as at a switch-off%s", more);
    else
        say("the car's Bluetooth name is given (LocalName, %u characters)%s", (unsigned)name.size(), more);
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

bool SppService::disconnectPhone(const std::string& device, const std::string& instance, const std::string& service,
                                 bool* waits) {
    if (waits != 0) *waits = false;
    pthread_mutex_lock(&mu_);
    std::string address = phoneAddress_;
    std::string named = service.empty() ? phoneService_ : service;
    pthread_mutex_unlock(&mu_);
    if (!gap_->attached() || address.empty()) return false;
    std::string d = device.empty() ? std::string(kDisconnectDevice) : device;
    std::string i = instance.empty() ? std::string(kDisconnectInstance) : instance;
    if (!takeDisconnect(address, named, false)) {
        if (waits != 0) *waits = true;
        return false;
    }
    say("ServiceDisconnect device '%s', instance '%s', service '%s', for the phone of the last request for a link",
        d.c_str(), i.c_str(), named.c_str());
    if (send(kReqServiceDisconnect, buildServiceDisconnect(d, i, address, named))) return true;
    pthread_mutex_lock(&mu_);
    unknownLeft_ = 0;
    disconnectOut_ = false;
    pthread_mutex_unlock(&mu_);
    return false;
}

// Takes the one ServiceDisconnect that may be out, for `address` and `service`: false, with nothing changed, while
// another waits for its answer. One that has waited disconnectReplyS_ is given up first. The log says so of
// disconnectPhone()'s; of disconnectService()'s the caller says it, as it waits for its answer the same time.
bool SppService::takeDisconnect(const std::string& address, const std::string& service, bool told, bool quiet) {
    pthread_mutex_lock(&mu_);
    double now = monoNow();
    bool lost = disconnectOut_ && now - disconnectSentAt_ >= disconnectReplyS_;
    bool sayLost = lost && !disconnectTold_;
    if (lost) disconnectOut_ = disconnectTold_ = false;
    bool busy = disconnectOut_;
    if (!busy) {
        unknownLeft_ = kUnknownLogged;
        disconnectOut_ = true;
        disconnectTold_ = told;
        disconnectQuiet_ = quiet;
        disconnectAddress_ = address;
        disconnectService_ = service;
        disconnectSentAt_ = now;
    }
    pthread_mutex_unlock(&mu_);
    if (sayLost) say("ServiceDisconnect had no reply in %.1f s, and is given up", disconnectReplyS_);
    return !busy;
}

bool SppService::connectService(const std::string& device, const std::string& instance, const std::string& address,
                                const std::string& service) {
    if (!gap_->attached()) return false;
    pthread_mutex_lock(&mu_);
    bool busy = connectOut_;
    // A kept connect that names the same is this one's from here: their answers cannot be told apart.
    bool replaces = !busy && lateKept_ && lateService_ == service && addressKey(lateAddress_) == addressKey(address);
    if (!busy) {
        connectOut_ = true;
        connectAddress_ = address;
        connectService_ = service;
        if (replaces) lateKept_ = false;
    }
    pthread_mutex_unlock(&mu_);
    if (busy) return false;
    if (send(kReqServiceConnect, buildServiceConnect(device, instance, address, service))) return true;
    pthread_mutex_lock(&mu_);
    connectOut_ = false;
    if (replaces) lateKept_ = true;
    pthread_mutex_unlock(&mu_);
    return false;
}

bool SppService::giveUpConnect(const std::string& address, const std::string& service, LateAnswer late) {
    pthread_mutex_lock(&mu_);
    bool ours = connectOut_ && connectService_ == service && addressKey(connectAddress_) == addressKey(address);
    if (ours) connectOut_ = false;
    if (ours && late == kLateTold) {
        lateKept_ = true;
        lateAddress_ = connectAddress_;
        lateService_ = connectService_;
    }
    pthread_mutex_unlock(&mu_);
    return ours;
}

bool SppService::queryServices(const std::string& address) {
    if (!gap_->attached()) return false;
    pthread_mutex_lock(&mu_);
    queryAddress_ = address;
    pthread_mutex_unlock(&mu_);
    if (send(kReqServiceListQuery, buildServiceListQuery(address))) return true;
    pthread_mutex_lock(&mu_);
    queryAddress_.clear();
    pthread_mutex_unlock(&mu_);
    return false;
}

bool SppService::disconnectService(const std::string& device, const std::string& instance, const std::string& address,
                                   const std::string& service, bool quiet) {
    if (!gap_->attached()) return false;
    if (!takeDisconnect(address, service, true, quiet)) return false;
    if (!quiet)
        say("ServiceDisconnect device '%s', instance '%s', service '%s'", device.c_str(), instance.c_str(), service.c_str());
    if (send(kReqServiceDisconnect, buildServiceDisconnect(device, instance, address, service))) return true;
    pthread_mutex_lock(&mu_);
    unknownLeft_ = 0;
    disconnectOut_ = false;
    disconnectTold_ = false;
    pthread_mutex_unlock(&mu_);
    return false;
}

namespace {
// ServiceDisconnected informs are logged one by one up to this many; after that they are only counted.
const unsigned kEndsLogged = 8;
// So are ServiceConnect and ServiceDisconnect replies for another client that come while ours waits.
const unsigned kOthersConnectLogged = 4;
const unsigned kOthersDisconnectLogged = 4;
// unreadSaid_: which kind of message has been logged once as one that could not be read.
enum { kUnreadDeviceList = 1, kUnreadEnded = 2, kUnreadServiceList = 4, kUnreadVersion = 8, kUnreadDisconnect = 16,
       kUnreadConnect = 32, kUnreadQuery = 64 };
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
        if (id == kReqServiceListQuery) {
            pthread_mutex_lock(&mu_);
            ServiceList l;
            l.error = kNoError;
            l.rejected = true;
            l.address = queryAddress_;
            queryAddress_.clear();
            pthread_mutex_unlock(&mu_);
            say("the Bluetooth server rejected ServiceListQuery (type 0x%x)", type);
            if (!l.address.empty()) events_->sppServiceList(l);
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
        if (l.ok && l.error == 0) events_->sppDevices(l.devices);
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
        if (d.ok) events_->sppLinkEnded(d);
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
            std::vector<std::string> shown;
            for (size_t i = 0; i < v.parts.size(); ++i) {
                shown.push_back(shownText(v.parts[i], 64));
                parts += (i ? ", '" : "'") + shown.back() + "'";
            }
            pthread_mutex_lock(&mu_);
            heard_.version = shown;
            pthread_mutex_unlock(&mu_);
            say("the Bluetooth stack's version: %s", parts.c_str());
        }
    } else if (id == kRspServiceDisconnect) {
        ServiceDisconnectReply d = parseServiceDisconnectReply(payload, len);
        pthread_mutex_lock(&mu_);
        bool out = disconnectOut_, told = disconnectTold_, quiet = disconnectQuiet_;
        // A reply read whole that names another phone is another client's, even while ours waits: ours stays out,
        // for its own answer. So is one that names another service than disconnectService() sent, which the car's
        // reply repeats; disconnectPhone()'s service is not settled, and is not compared.
        bool others = out && d.ok &&
                      (addressKey(d.address) != addressKey(disconnectAddress_) || (told && d.service != disconnectService_));
        unsigned n = 0;
        if (others) n = ++heard_.othersDisconnectReplies;
        else disconnectOut_ = disconnectTold_ = false;
        std::string address = disconnectAddress_, service = disconnectService_;
        pthread_mutex_unlock(&mu_);
        if (!out) return true;     // another client's
        if (others) {
            if (n <= kOthersDisconnectLogged)
                say("a ServiceDisconnect reply for another client came while the agent's ServiceDisconnect waited for "
                    "its answer, and was left: the agent's waits on%s",
                    n == kOthersDisconnectLogged ? ". Further ones are counted and not logged" : "");
            return true;
        }
        if (!d.ok) {
            unread = kUnreadDisconnect;
            what = "ServiceDisconnect's reply";
        } else if (!quiet) {
            say("ServiceDisconnect reply: %s, reason %u", gapErrorText(d.error, "").c_str(), d.reason);
        }
        if (told) {
            if (!d.ok) {
                d.address = address;
                d.service = service;
                d.error = kNoError;
            }
            events_->sppDisconnectReply(d);
        }
    } else if (id == kRspServiceConnect) {
        ServiceConnectReply c = parseServiceConnectReply(payload, len);
        pthread_mutex_lock(&mu_);
        bool out = connectOut_;
        // A reply read whole that names another service or another phone is another client's, even while ours
        // waits: ours stays out, for its own answer.
        bool others =
            out && c.ok && (c.service != connectService_ || addressKey(c.address) != addressKey(connectAddress_));
        unsigned n = 0;
        if (others) n = ++heard_.othersConnectReplies;
        else connectOut_ = false;
        std::string address = connectAddress_, service = connectService_;
        // The late answer to the connect kept by giveUpConnect(kLateTold): told only while none of ours waits, and
        // forgotten either way once its reply has come.
        bool namesKept =
            lateKept_ && c.ok && c.service == lateService_ && addressKey(c.address) == addressKey(lateAddress_);
        if (namesKept && (!out || others)) lateKept_ = false;
        bool late = namesKept && !out;
        pthread_mutex_unlock(&mu_);
        if (late) {
            events_->sppLateConnectReply(c);
            return true;
        }
        if (!out) return true;     // another client's
        if (others) {
            if (n <= kOthersConnectLogged)
                say("a ServiceConnect reply for another client came while the agent's dial waited for its answer, and "
                    "was left: the dial waits on%s",
                    n == kOthersConnectLogged ? ". Further ones are counted and not logged" : "");
            return true;
        }
        if (!c.ok) {
            c.address = address;
            c.service = service;
            c.error = kNoError;
            unread = kUnreadConnect;
            what = "ServiceConnect's reply";
        }
        events_->sppConnectReply(c);
    } else if (id == kRspServiceListQuery) {
        // Told whoever's query it answers: which ask it is the answer to is for the caller to say. One that names no
        // phone, or cannot be read, is taken as the answer to ours, if one waits.
        ServiceList l = parseServiceList(payload, len);
        pthread_mutex_lock(&mu_);
        std::string asked = queryAddress_;
        bool named = l.ok && !l.address.empty();
        if (!named || addressKey(l.address) == addressKey(asked)) queryAddress_.clear();
        pthread_mutex_unlock(&mu_);
        if (!l.ok) {
            l = ServiceList();
            l.error = kNoError;
            unread = kUnreadQuery;
            what = "ServiceListQuery's reply";
        }
        if (!named) l.address = asked;
        events_->sppServiceList(l);
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
    // A server that has just attached, a new one too: Status is not known yet, and the hold counts from here if it
    // says the stack is open (When registrations may go, in the header).
    pthread_mutex_lock(&mu_);
    attachedAt_ = now();
    bt_ = kBtUnknown;
    statusHeard_ = false;
    openSince_ = 0;
    clearError_ = kNoError;
    pthread_mutex_unlock(&mu_);
    subscribe();   // Status may come during this, and is kept
    // Held, and the round begun at once if it may be, in one step: no other thread begins it in between.
    std::string line;
    pthread_mutex_lock(&mu_);
    State before = state_;
    state_ = kHeld;
    bool begin = beginLocked(&line);
    pthread_mutex_unlock(&mu_);
    // kHeld is told only where the round cannot begin at once.
    if (begin) begun(line);
    else if (before != kHeld) events_->sppState(kHeld);
}

void SppService::gapDetached() {
    say("the Bluetooth server detached (its process stopped or restarted): the registration is gone with it");
    pthread_mutex_lock(&mu_);
    bool had = state_ == kRegSent || state_ == kClearSent || state_ == kRetrySent || state_ == kRegistered ||
               state_ == kHeld;
    if (had) state_ = kWaiting;
    subscribed_ = false;
    // Every registration went with the server, the stand-ins' and the ones that removeOther() was asked to remove
    // among them, and no answer comes from it to one that is out.
    forgetRegistrationsLocked();
    regFor_ = kForNone;
    regStale_ = false;
    // No answer comes from a server that has gone; a new one starts its services as its settings say.
    askOut_ = askStale_ = setOut_ = setStale_ = false;
    rejectOut_ = false;
    keptWasOn_ = false;
    bt_ = kBtUnknown;
    statusHeard_ = false;
    otherSent_.clear();
    // No reply comes from a server that has gone, and what it said of the phones is old.
    disconnectOut_ = false;
    disconnectTold_ = false;
    connectOut_ = false;
    lateKept_ = false;
    queryAddress_.clear();
    refusalAsked_ = false;
    versionAsked_ = false;
    heard_.devicesKnown = false;
    heard_.devices.clear();
    pthread_mutex_unlock(&mu_);
    if (had) events_->sppState(kWaiting);
}

void SppService::gapMessage(uint32_t type, uint32_t id, const uint8_t* payload, uint32_t len) {
    // An attribute's messages have the type of a rejection: by their id first.
    if (isAttributeId(id)) {
        if (id == kAttrStatus) statusTold(payload, len);
        else nameTold(payload, len);
        return;
    }
    if (isServiceRequestInform(id)) {
        // The phone's name and address are personal data: they stay here, and only the service goes on. The address
        // is kept, for a ServiceDisconnect, and never said.
        ServiceRequest q = parseServiceRequest(id, payload, len);
        // A request for the knock service is a knock, never a link's: it goes its own way, and is kept in nothing
        // below.
        if (!knockService_.empty() && q.service == knockService_) {
            events_->sppKnockRequest(id == kInfServiceRequest ? kPhoneAsked
                                                              : (id == kInfServiceRequestComplete ? kPhoneLinked : kPhoneRefused),
                                     q.address, q.ok);
            return;
        }
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
                          q.service, q.ok, id == kInfServiceRequest && q.ok ? q.address : std::string());
        return;
    }
    bool rejected = type == kTypeRejectedA || type == kTypeRejectedB;
    if (rejected && id == kReqServiceDisconnect) {
        pthread_mutex_lock(&mu_);
        bool told = disconnectTold_;
        disconnectOut_ = disconnectTold_ = false;
        ServiceDisconnectReply d;
        d.address = disconnectAddress_;
        d.service = disconnectService_;
        d.error = kNoError;
        d.reason = 0;
        d.ok = false;
        pthread_mutex_unlock(&mu_);
        say("the Bluetooth server rejected ServiceDisconnect (type 0x%x)", type);
        if (told) events_->sppDisconnectReply(d);
        return;
    }
    if (rejected && id == kReqServiceConnect) {
        pthread_mutex_lock(&mu_);
        bool out = connectOut_;
        connectOut_ = false;
        ServiceConnectReply c;
        c.address = connectAddress_;
        c.service = connectService_;
        c.error = kNoError;
        c.ok = false;
        pthread_mutex_unlock(&mu_);
        say("the Bluetooth server rejected ServiceConnect (type 0x%x)", type);
        if (out) events_->sppConnectReply(c);
        return;
    }
    if (!rejected && isPairingInform(id)) {
        events_->sppPairing(id, pairingInformAddress(payload, len));
        return;
    }
    // Not asked for, and a pairing that worked stops nothing: one that comes all the same is dropped, unsaid.
    if (!rejected && id == kInfPairingComplete) return;
    if (handleKnown(type, id, payload, len)) return;

    // The kept service's look and switch, and a refusal of a request: before anything that takes a reply by its
    // order, or a rejection for the registration's.
    if ((!rejected && (id == kRspSupportedServicesGet || id == kRspEnabledServicesSet)) ||
        (rejected && (id == kReqSupportedServicesGet || id == kReqEnabledServicesSet))) {
        keptAnswered(id, rejected ? type : 0, payload, len);
        return;
    }
    if ((!rejected && id == kRspServiceReject) || (rejected && id == kReqServiceReject)) {
        pthread_mutex_lock(&mu_);
        bool out = rejectOut_;
        rejectOut_ = false;
        pthread_mutex_unlock(&mu_);
        if (out) events_->sppRejectAnswered(rejected ? kNoError : readError(payload, len));
        return;   // with none of ours out, another client's
    }

    // The answer to a removal of another program's registration, if one is out: it went out before any removal of
    // our own that is waiting too, so its answer is the first of the two.
    if ((id == kRspSppUuidDeregister && !rejected) || (id == kReqSppUuidDeregister && rejected)) {
        pthread_mutex_lock(&mu_);
        std::string other = otherSent_;
        OtherWhy why = otherSentWhy_;
        otherSent_.clear();
        pthread_mutex_unlock(&mu_);
        if (!other.empty()) {
            uint32_t err = rejected ? kNoError : readError(payload, len);
            if (rejected) say("the Bluetooth server rejected the removal of '%s' (type 0x%x)", other.c_str(), type);
            else say("SppUuidDeregister reply for '%s': %s", other.c_str(), deregisterErrorText(err).c_str());
            events_->sppOtherRemoved(other, err);
            otherAnswered(Other(other, why), err);
            sendNextOther();
            return;
        }
    }

    // Whichever SppUuidRegister of ours was out: the stand-in's, the check's or our own.
    if ((id == kRspSppUuidRegister && !rejected) || (id == kReqSppUuidRegister && rejected)) {
        registerAnswered(rejected ? kNoError : readError(payload, len), rejected ? type : 0);
        return;
    }

    State s = state();
    if (rejected) {
        say("the Bluetooth server rejected the request (type 0x%x, id 0x%x)", type, id);
        if (s == kClearSent) {
            set(kRetrySent);
            pthread_mutex_lock(&mu_);
            retryDue_ = true;
            pthread_mutex_unlock(&mu_);
            pump();
        }
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
        if (s == kClearSent) {
            set(kRetrySent);
            pthread_mutex_lock(&mu_);
            retryDue_ = true;
            pthread_mutex_unlock(&mu_);
            pump();
        }
        else set(err == 0 || err == 10 ? kRemoved : kRemoveFailed);
        sendNextOther();   // a removal of another program's registration may have waited for this answer
        return;
    }
    if (type == kTypeResponse && id != kRspSppUuidRegister && id != kRspSppUuidDeregister) noteUnknown(type, id, payload, len);
}

}  // namespace btspp
}  // namespace nbt

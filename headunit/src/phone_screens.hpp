// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBTAPP_PHONE_SCREENS_HPP
#define NBTAPP_PHONE_SCREENS_HPP

// What the plugin's screens do, without the HU Framework: they talk to the agent and tell a view what to show.
// src/nbt_app.cpp is the view, and turns a status into a text of the text DB; a test is another
// (tests/agent/test_plugin.cpp), against a real agent.
//
//   Start the agent     Shown: asks whether an agent answers on its socket, and says so. With none, its second
//                       row starts the launcher (agent_start.hpp, startByHand) and the screen asks again, once a
//                       second, until an agent answers or kStartWaitTicks have gone by. The way to start an agent
//                       on a head unit with no console, so that a phone can reach it.
//   Stop the agent      Says what it does, and on a press of its second row asks the agent to stop (agent.stop), as
//                       `nbtctl stop` does, and keeps its link until the agent has ended, kStopWaitTicks at most. The
//                       agent leaves a mark that keeps the plugin from starting it with the head unit until the head
//                       unit has powered down, or Start the agent starts it (agent_start.hpp).
//
//   Pair a phone        Shown: asks the agent for a pairing window (pairing.open) and shows its code. The user types
//                       the code into the phone app, the agent hands that phone its key, and the screen says so.
//                       Left: the window is closed (pairing.close, and the end of the link). The window is open
//                       only while the code is on the screen.
//   Forget all phones   Asks once, on a row of its own, and then tells the agent to forget every paired phone
//                       (phones.forget). Each phone has to be paired again after that.
//   Reset Bluetooth     Says what it does, and on a press of its second row asks the agent to have the car end the
//   link                phone's Bluetooth serial link (serial.reset), for a phone app that connects and gets nothing.
//                       With a link up the car is asked to end it; with none, the next link that opens within a minute
//                       is ended at its open, and the screen says to tap Connect in the phone app for that.
//
// A screen opens a link to the agent when it has something to ask and closes it when it has its answer, so
// with none of them on the screen the plugin has no link and runs no timer. Start the agent also runs the timer
// while it waits for an agent it has started, and only while it is shown. Stop the agent keeps its link after the
// answer until the agent has ended, and only while it is shown. tick() is the timer: the app calls it
// every kScreenTickMs for as long as wantsTick() is true, and all of a screen's talk with the agent happens inside
// it.
// An agent that does not answer within kScreenAnswerTicks of them is taken for not running.
//
// A screen is driven from the app's one thread, which is the HMI's thread that drives the iDrive screen as well:
// a call of a screen's that waited would stop all of iDrive. MenuScreens, at the end of this file, is the five
// together as the app drives them, and it has a press of a row do its work in a tick of its own, posted at once, so
// that the HMI's handler of the press returns at once. open() of a link is not called from inside a link's
// callback.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string>

#include "agent_link.hpp"

namespace nbt {

const unsigned kScreenTickMs = 200;
const unsigned kScreenAnswerTicks = 25;   // 5 s
const unsigned kStartProbeTicks = 5;      // 1 s: how often Start the agent asks again after a start
const unsigned kStartWaitTicks = 150;     // 30 s: how long it asks before it says the start did not work
const unsigned kStopWaitTicks = 150;      // 30 s: how long Stop the agent waits for the agent to end after its yes

// ---- Pair a phone --------------------------------------------------------------------------------------------------

enum PairStatus {
    kPairAsking,       // the agent has been asked for a code
    kPairCode,         // the code is on the screen
    kPairDone,         // a phone typed it and was paired
    kPairWrongCodes,   // wrong codes were typed: the window is closed, and there is no new one for a while
    kPairRanOut,       // nobody typed the code in time, or another controller took the window over
    kPairTooSoon,      // the pause after wrong codes is still running
    kPairNoAgent,      // no agent is running, or it stopped
    kPairFailed        // the agent answered with something else
};

class PairView {
public:
    virtual ~PairView() {}
    // `code` is the six digits with kPairCode, and empty otherwise: a code that is no longer good is taken off
    // the screen.
    virtual void pairShow(PairStatus status, const std::string& code) = 0;
};

class PairScreen : public AgentLinkListener {
public:
    PairScreen(const std::string& socketPath, PairView* view)
        : path_(socketPath), view_(view), link_(this), openId_(0), waited_(0), status_(kPairNoAgent) {}

    // The screen came up: a new window, whatever was there before.
    void shown() {
        link_.close();
        openId_ = 0;
        waited_ = 0;
        show(kPairAsking, "");
        std::string why;
        if (!link_.open(path_, &why)) {
            show(kPairNoAgent, "");
            return;
        }
        openId_ = link_.request("pairing.open", "{}");
    }

    // The screen was left: the code is good for nothing any more.
    void hidden() {
        if (link_.isOpen()) link_.request("pairing.close", "{}");   // written out at once; the link's end says the same
        link_.close();
        openId_ = 0;
    }

    bool wantsTick() const { return link_.isOpen(); }

    void tick() {
        link_.poll();
        if (openId_ != 0 && link_.isOpen() && ++waited_ > kScreenAnswerTicks) {
            link_.close();
            openId_ = 0;
            show(kPairNoAgent, "");
        }
    }

    PairStatus status() const { return status_; }

    // AgentLinkListener
    virtual void agentReply(int64_t id, const json::Value& result, const std::string& code) {
        if (id != openId_) return;   // the answer to pairing.close, or to a window that was asked for before
        openId_ = 0;
        json::Value digits = result.find("code");
        if (code.empty() && digits.isString() && isCode(digits.string())) {
            show(kPairCode, digits.string());
            return;
        }
        link_.close();
        show(code == "try-later" ? kPairTooSoon : kPairFailed, "");
    }
    virtual void agentEvent(const std::string& name, const json::Value& params) {
        if (name != "pairing.closed" || status_ != kPairCode) return;
        json::Value reason = params.find("reason");
        std::string why = reason.isString() ? reason.string() : std::string();
        link_.close();
        show(why == "issued" ? kPairDone : why == "wrong-code" ? kPairWrongCodes : kPairRanOut, "");
    }
    virtual void agentGone(const std::string&) {
        openId_ = 0;
        // What was said last stays, if it was the end of a window: the agent may stop after it.
        if (status_ == kPairAsking || status_ == kPairCode) show(kPairNoAgent, "");
    }

private:
    static bool isCode(const std::string& text) {
        if (text.size() != kPairingCodeDigits) return false;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
        }
        return true;
    }
    void show(PairStatus status, const std::string& code) {
        status_ = status;
        view_->pairShow(status, code);
    }

    std::string path_;
    PairView* view_;
    AgentLink link_;
    int64_t openId_;       // the pairing.open that has no answer yet, or 0
    unsigned waited_;      // ticks since it was sent
    PairStatus status_;
};

// ---- Forget all phones ---------------------------------------------------------------------------------------------

enum ForgetStatus {
    kForgetAsk,        // the row that does it is waiting to be pressed
    kForgetWorking,    // the agent has been told
    kForgetDone,       // no phone is paired any more
    kForgetNoAgent,    // no agent is running, or it stopped
    kForgetFailed      // the agent answered with an error
};

class ForgetView {
public:
    virtual ~ForgetView() {}
    virtual void forgetShow(ForgetStatus status) = 0;
};

class ForgetScreen : public AgentLinkListener {
public:
    ForgetScreen(const std::string& socketPath, ForgetView* view)
        : path_(socketPath), view_(view), link_(this), forgetId_(0), waited_(0), status_(kForgetAsk) {}

    // The screen came up: it asks again, whatever the last time came to.
    void shown() {
        link_.close();
        forgetId_ = 0;
        show(kForgetAsk);
    }

    void hidden() {
        // What was sent is not taken back: the agent forgets, and nobody is told. A press that had no answer is
        // over, so the next showing starts over; the app starts no showing over while one waits.
        link_.close();
        forgetId_ = 0;
        if (status_ == kForgetWorking) status_ = kForgetAsk;
    }

    // The row was pressed. Once per showing: a second press does nothing.
    void confirmed() {
        if (status_ != kForgetAsk) return;
        waited_ = 0;
        std::string why;
        if (!link_.open(path_, &why)) {
            show(kForgetNoAgent);
            return;
        }
        show(kForgetWorking);
        forgetId_ = link_.request("phones.forget", "{\"all\":true}");
    }

    bool wantsTick() const { return link_.isOpen(); }

    void tick() {
        link_.poll();
        if (forgetId_ != 0 && link_.isOpen() && ++waited_ > kScreenAnswerTicks) {
            link_.close();
            forgetId_ = 0;
            show(kForgetNoAgent);
        }
    }

    ForgetStatus status() const { return status_; }

    // AgentLinkListener
    virtual void agentReply(int64_t id, const json::Value&, const std::string& code) {
        if (id != forgetId_) return;
        forgetId_ = 0;
        link_.close();
        show(code.empty() ? kForgetDone : kForgetFailed);
    }
    virtual void agentEvent(const std::string&, const json::Value&) {}
    virtual void agentGone(const std::string&) {
        forgetId_ = 0;
        if (status_ == kForgetWorking) show(kForgetNoAgent);
    }

private:
    void show(ForgetStatus status) {
        status_ = status;
        view_->forgetShow(status);
    }

    std::string path_;
    ForgetView* view_;
    AgentLink link_;
    int64_t forgetId_;     // the phones.forget that has no answer yet, or 0
    unsigned waited_;
    ForgetStatus status_;
};

// ---- Reset Bluetooth link ------------------------------------------------------------------------------------------

enum ResetStatus {
    kResetAsk,           // the row that does it is waiting to be pressed
    kResetWorking,       // the agent has been asked
    kResetArmed,         // no serial link was up: the next one within a minute is ended at its open
    kResetSent,          // a serial link was up, and the car has been asked to end it
    kResetNoAgent,       // no agent is running, or it stopped
    kResetBusy,          // the agent is running a job, and was not asked
    kResetUnsupported,   // the agent has no serial.reset: one from before it, or one with no Bluetooth serial listener
    kResetFailed         // the agent answered with another error, or with a result this screen does not know
};

class ResetView {
public:
    virtual ~ResetView() {}
    virtual void resetShow(ResetStatus status) = 0;
};

// The agent answers serial.reset at once, and the car ends the link after that: what the screen says is what the
// agent answered, and the agent's log says what came of it. An agent that does not know the method answers it with
// `unsupported`, as one does that has the method and no Bluetooth serial listener; the screen tells neither apart.
class ResetScreen : public AgentLinkListener {
public:
    ResetScreen(const std::string& socketPath, ResetView* view)
        : path_(socketPath), view_(view), link_(this), resetId_(0), waited_(0), status_(kResetAsk) {}

    // The screen came up: it says what it does again, whatever the last time came to.
    void shown() {
        link_.close();
        resetId_ = 0;
        show(kResetAsk);
    }

    void hidden() {
        // What was sent is not taken back: the car ends the link, or the next one, and nobody is told. A press that
        // had no answer is over, so the next showing starts over; the app starts no showing over while one waits.
        link_.close();
        resetId_ = 0;
        if (status_ == kResetWorking) status_ = kResetAsk;
    }

    // The row was pressed. Once per showing: a second press does nothing.
    void pressed() {
        if (status_ != kResetAsk) return;
        waited_ = 0;
        std::string why;
        if (!link_.open(path_, &why)) {
            show(kResetNoAgent);
            return;
        }
        show(kResetWorking);
        // With no device, instance or service: the agent puts in its own.
        resetId_ = link_.request("serial.reset", "{}");
    }

    bool wantsTick() const { return link_.isOpen(); }

    void tick() {
        link_.poll();
        if (resetId_ != 0 && link_.isOpen() && ++waited_ > kScreenAnswerTicks) {
            link_.close();
            resetId_ = 0;
            show(kResetNoAgent);
        }
    }

    ResetStatus status() const { return status_; }

    // AgentLinkListener
    virtual void agentReply(int64_t id, const json::Value& result, const std::string& code) {
        if (id != resetId_) return;
        resetId_ = 0;
        link_.close();
        if (!code.empty()) {
            show(code == "busy" ? kResetBusy : code == "unsupported" ? kResetUnsupported : kResetFailed);
            return;
        }
        json::Value state = result.find("state");
        std::string said = state.isString() ? state.string() : std::string();
        show(said == "sent" ? kResetSent : said == "armed" ? kResetArmed : kResetFailed);
    }
    virtual void agentEvent(const std::string&, const json::Value&) {}
    virtual void agentGone(const std::string&) {
        resetId_ = 0;
        if (status_ == kResetWorking) show(kResetNoAgent);
    }

private:
    void show(ResetStatus status) {
        status_ = status;
        view_->resetShow(status);
    }

    std::string path_;
    ResetView* view_;
    AgentLink link_;
    int64_t resetId_;      // the serial.reset that has no answer yet, or 0
    unsigned waited_;
    ResetStatus status_;
};

// ---- Stop the agent ------------------------------------------------------------------------------------------------

enum StopStatus {
    kStopAsk,           // the row that does it is waiting to be pressed
    kStopWorking,       // the agent has been asked, and has not answered
    kStopStopping,      // it answered: it is ending, and the link is kept until it has
    kStopStopped,       // the agent has ended
    kStopNoAgent,       // no agent is running, or it went before it answered
    kStopBusy,          // the agent is running a job, or a restart is on its way, and was not stopped
    kStopUnsupported,   // the agent has no agent.stop: one from before it
    kStopFailed         // the agent answered with another error, or had not ended kStopWaitTicks after its answer
};

class StopView {
public:
    virtual ~StopView() {}
    virtual void stopShow(StopStatus status) = 0;
};

// The agent answers agent.stop at once and ends once its answer is written out. It leaves the local socket's links
// open until its process ends (agent/agent.hpp), so the screen keeps its link after the answer, and the end of the
// link is what says that the agent has stopped. An agent that does not know the method answers it with
// `unsupported`. What keeps the plugin from starting the stopped agent again is in agent_start.hpp.
class StopScreen : public AgentLinkListener {
public:
    StopScreen(const std::string& socketPath, StopView* view)
        : path_(socketPath), view_(view), link_(this), stopId_(0), waited_(0), status_(kStopAsk) {}

    // The screen came up: it says what it does again, whatever the last time came to.
    void shown() {
        link_.close();
        stopId_ = 0;
        show(kStopAsk);
    }

    void hidden() {
        // What was sent is not taken back: the agent stops, and nobody is told. A press that had no answer, or whose
        // agent had not ended yet, is over, so the next showing starts over; the app starts no showing over while
        // one waits.
        link_.close();
        stopId_ = 0;
        if (status_ == kStopWorking || status_ == kStopStopping) status_ = kStopAsk;
    }

    // The row was pressed. Once per showing: a second press does nothing.
    void pressed() {
        if (status_ != kStopAsk) return;
        waited_ = 0;
        std::string why;
        if (!link_.open(path_, &why)) {
            show(kStopNoAgent);
            return;
        }
        show(kStopWorking);
        stopId_ = link_.request("agent.stop", "{}");
    }

    bool wantsTick() const { return link_.isOpen(); }

    void tick() {
        // The tick that brings the answer is not one of the kStopWaitTicks.
        bool stopping = status_ == kStopStopping;
        link_.poll();
        if (!link_.isOpen()) return;
        if (status_ == kStopWorking && stopId_ != 0 && ++waited_ > kScreenAnswerTicks) {
            link_.close();
            stopId_ = 0;
            show(kStopNoAgent);
        } else if (stopping && status_ == kStopStopping && ++waited_ >= kStopWaitTicks) {
            // It said yes, and is still there.
            link_.close();
            show(kStopFailed);
        }
    }

    StopStatus status() const { return status_; }

    // AgentLinkListener
    virtual void agentReply(int64_t id, const json::Value&, const std::string& code) {
        if (id != stopId_) return;
        stopId_ = 0;
        if (code.empty()) {
            // The link stays: its end is the agent's.
            waited_ = 0;
            show(kStopStopping);
            return;
        }
        link_.close();
        show(code == "busy" ? kStopBusy : code == "unsupported" ? kStopUnsupported : kStopFailed);
    }
    virtual void agentEvent(const std::string&, const json::Value&) {}
    virtual void agentGone(const std::string&) {
        stopId_ = 0;
        if (status_ == kStopStopping) show(kStopStopped);
        else if (status_ == kStopWorking) show(kStopNoAgent);
    }

private:
    void show(StopStatus status) {
        status_ = status;
        view_->stopShow(status);
    }

    std::string path_;
    StopView* view_;
    AgentLink link_;
    int64_t stopId_;       // the agent.stop that has no answer yet, or 0
    unsigned waited_;      // ticks since it was sent, and then since it was answered
    StopStatus status_;
};

// ---- Start the agent -----------------------------------------------------------------------------------------------

enum StartStatus {
    kStartChecking,     // asking whether an agent answers
    kStartRunning,      // one answers: there is nothing to start
    kStartAsk,          // none answers: the row that starts it is waiting to be pressed
    kStartStarting,     // the launcher was started, or one was running: asking until an agent answers
    kStartStarted,      // an agent answered after the start
    kStartNoLauncher,   // there is no launcher in the agent's folder: the agent is not installed
    kStartFailed        // the launcher could not be started, or no agent answered in time
};

// What a start of the launcher came to: agent_start.hpp's Decision, with only what this screen tells apart.
enum LaunchResult {
    kLaunched,          // the launcher was started
    kLaunchRunning,     // a launcher is running already: its agent may be on its way, or between two starts
    kLaunchNoLauncher,  // there is no launcher
    kLaunchFailed       // the launcher could not be started
};

// What starts the launcher: agentstart::startByHand in the plugin, something else in a test.
class AgentStarter {
public:
    virtual ~AgentStarter() {}
    virtual LaunchResult startAgent() = 0;
};

class StartView {
public:
    virtual ~StartView() {}
    virtual void startShow(StartStatus status) = 0;
};

// An agent answers when it takes the link and the plugin's hello, and answers a ping after that. Whatever listens
// on the socket is an agent, and a second one started beside it ends at once (agent/link.cpp), so a start with one
// there would do no harm; it is not offered all the same, so that the screen says what is so.
class StartScreen : public AgentLinkListener {
public:
    StartScreen(const std::string& socketPath, StartView* view, AgentStarter* starter)
        : path_(socketPath), view_(view), starter_(starter), link_(this), pingId_(0), answerWait_(0), waited_(0),
          status_(kStartAsk) {}

    // The screen came up: it asks again, whatever the last time came to.
    void shown() {
        link_.close();
        pingId_ = 0;
        show(kStartChecking);
        if (!ask()) show(kStartAsk);
    }

    // A launcher that was started goes on; the screen stops asking after its agent.
    void hidden() {
        link_.close();
        pingId_ = 0;
        if (status_ == kStartChecking || status_ == kStartStarting) status_ = kStartAsk;
    }

    // The row was pressed: the launcher is started where no agent answers, and again after a start that failed.
    void pressed() {
        if (status_ != kStartAsk && status_ != kStartFailed) return;
        switch (starter_->startAgent()) {
            case kLaunched:
            case kLaunchRunning:
                waited_ = 0;
                show(kStartStarting);
                break;
            case kLaunchNoLauncher: show(kStartNoLauncher); break;
            case kLaunchFailed: show(kStartFailed); break;
        }
    }

    bool wantsTick() const { return link_.isOpen() || status_ == kStartStarting; }

    void tick() {
        link_.poll();
        if (pingId_ != 0 && link_.isOpen() && ++answerWait_ > kScreenAnswerTicks) {
            // It took the link and said nothing: no agent that can be talked to.
            link_.close();
            pingId_ = 0;
            if (status_ == kStartChecking) show(kStartAsk);
        }
        if (status_ != kStartStarting) return;
        ++waited_;
        if (!link_.isOpen() && waited_ % kStartProbeTicks == 0) ask();
        if (status_ == kStartStarting && waited_ >= kStartWaitTicks) {
            link_.close();
            pingId_ = 0;
            show(kStartFailed);
        }
    }

    StartStatus status() const { return status_; }

    // AgentLinkListener
    virtual void agentReply(int64_t id, const json::Value&, const std::string&) {
        if (id != pingId_) return;
        // Whatever it answered, an agent answered.
        pingId_ = 0;
        link_.close();
        if (status_ == kStartChecking) show(kStartRunning);
        else if (status_ == kStartStarting) show(kStartStarted);
    }
    virtual void agentEvent(const std::string&, const json::Value&) {}
    virtual void agentGone(const std::string&) {
        pingId_ = 0;
        // After a start the agent may be between two of its launcher's starts, and is asked again.
        if (status_ == kStartChecking) show(kStartAsk);
    }

private:
    // Opens the link and sends the ping; false if nobody listens.
    bool ask() {
        std::string why;
        if (!link_.open(path_, &why)) return false;
        answerWait_ = 0;
        pingId_ = link_.request("ping", "{}");
        if (pingId_ == 0) {
            link_.close();
            return false;
        }
        return true;
    }
    void show(StartStatus status) {
        status_ = status;
        view_->startShow(status);
    }

    std::string path_;
    StartView* view_;
    AgentStarter* starter_;
    AgentLink link_;
    int64_t pingId_;        // the ping that has no answer yet, or 0
    unsigned answerWait_;   // ticks since it was sent
    unsigned waited_;       // ticks since the start
    StartStatus status_;
};

// ---- the menu's screens together -----------------------------------------------------------------------------------

// The app's timer, as the screens see it: one tick, `ms` from now. MenuScreens has one on its way at most.
class TickTimer {
public:
    virtual ~TickTimer() {}
    virtual void post(unsigned ms) = 0;
};

// The five screens as the app drives them: what it hears from the HMI, and its timer.
//
// A press of a row that does something (Forget them now, Start, Reset now, Stop it now) only records itself, and
// asks for a tick at once (0 ms); that tick does what the press asks, and nothing else. So the HMI's handler of a
// press returns at once, whatever the press leads to: the HMI runs it on the thread that drives the iDrive screen.
// A second press of a row while its first waits for that tick is the same press, and one that comes while a tick
// is on its way is done by that tick. A press whose screen is left before its tick has come is dropped: it was
// a press on that showing.
class MenuScreens {
public:
    MenuScreens(const std::string& socketPath, PairView* pairView, ForgetView* forgetView, StartView* startView,
                ResetView* resetView, StopView* stopView, AgentStarter* starter, TickTimer* timer)
        : timer_(timer), pair_(socketPath, pairView), forget_(socketPath, forgetView),
          start_(socketPath, startView, starter), reset_(socketPath, resetView), stop_(socketPath, stopView),
          queued_(false), forgetPress_(false), startPress_(false), resetPress_(false), stopPress_(false) {}

    // A state of ours is shown (or has the focus), or was left.
    void pairShown() {
        // Told twice, as visible and as focused, it is one showing: the code on the screen stays.
        if (!pair_.wantsTick() || (pair_.status() != kPairAsking && pair_.status() != kPairCode)) pair_.shown();
        arm();
    }
    void pairHidden() { pair_.hidden(); }
    void forgetShown() {
        if (forget_.status() != kForgetWorking) forget_.shown();
    }
    void forgetHidden() {
        forgetPress_ = false;
        forget_.hidden();
    }
    void startShown() {
        start_.shown();
        arm();
    }
    void startHidden() {
        startPress_ = false;
        start_.hidden();
    }
    void resetShown() {
        if (reset_.status() != kResetWorking) reset_.shown();
    }
    void resetHidden() {
        resetPress_ = false;
        reset_.hidden();
    }
    void stopShown() {
        // Told twice while it waits for the agent, it is one showing.
        if (stop_.status() != kStopWorking && stop_.status() != kStopStopping) stop_.shown();
    }
    void stopHidden() {
        stopPress_ = false;
        stop_.hidden();
    }
    // The app is being taken down: no link to the agent outlasts it, and no press is done after it.
    void closeLinks() {
        forgetPress_ = startPress_ = resetPress_ = stopPress_ = false;
        pair_.hidden();
        forget_.hidden();
        start_.hidden();
        reset_.hidden();
        stop_.hidden();
    }

    // A row was pressed: recorded, for the next tick.
    void forgetPressed() {
        forgetPress_ = true;
        arm();
    }
    void startPressed() {
        startPress_ = true;
        arm();
    }
    void resetPressed() {
        resetPress_ = true;
        arm();
    }
    void stopPressed() {
        stopPress_ = true;
        arm();
    }

    // The tick the app was asked for has come. One that has presses to do does those and no screen's tick, so that a
    // screen's first tick after a press comes kScreenTickMs after it, as its waits count on.
    void tick() {
        queued_ = false;
        if (!press()) {
            pair_.tick();
            forget_.tick();
            start_.tick();
            reset_.tick();
            stop_.tick();
        }
        arm();
    }

    const PairScreen& pair() const { return pair_; }
    const ForgetScreen& forget() const { return forget_; }
    const StartScreen& start() const { return start_; }
    const ResetScreen& reset() const { return reset_; }
    const StopScreen& stop() const { return stop_; }

private:
    MenuScreens(const MenuScreens&);
    MenuScreens& operator=(const MenuScreens&);

    // Does the presses that are recorded; whether there were any. A screen's own status check is what ignores a
    // press that has nothing to do, as it did when the press was done in the HMI's handler.
    bool press() {
        bool any = forgetPress_ || startPress_ || resetPress_ || stopPress_;
        if (forgetPress_) {
            forgetPress_ = false;
            forget_.confirmed();
        }
        if (startPress_) {
            startPress_ = false;
            start_.pressed();
        }
        if (resetPress_) {
            resetPress_ = false;
            reset_.pressed();
        }
        if (stopPress_) {
            stopPress_ = false;
            stop_.pressed();
        }
        return any;
    }

    // Keeps the timer running for as long as a screen has a link to the agent, or a press waits, and not a tick
    // longer. A press is done at once; a screen is ticked every kScreenTickMs.
    void arm() {
        if (queued_) return;
        bool pressed = forgetPress_ || startPress_ || resetPress_ || stopPress_;
        if (!pressed &&
            !(pair_.wantsTick() || forget_.wantsTick() || start_.wantsTick() || reset_.wantsTick() || stop_.wantsTick())) {
            return;
        }
        queued_ = true;
        timer_->post(pressed ? 0 : kScreenTickMs);
    }

    TickTimer* timer_;
    PairScreen pair_;
    ForgetScreen forget_;
    StartScreen start_;
    ResetScreen reset_;
    StopScreen stop_;
    bool queued_;          // a tick is on its way
    bool forgetPress_;     // presses that wait for it
    bool startPress_;
    bool resetPress_;
    bool stopPress_;
};

}  // namespace nbt

#endif

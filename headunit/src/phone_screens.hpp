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
//
//   Pair a phone        Shown: asks the agent for a pairing window (pairing.open) and shows its code. The user types
//                       the code into the phone app, the agent hands that phone its key, and the screen says so.
//                       Left: the window is closed (pairing.close, and the end of the link). The window is open
//                       only while the code is on the screen.
//   Forget all phones   Asks once, on a row of its own, and then tells the agent to forget every paired phone
//                       (phones.forget). Each phone has to be paired again after that.
//
// A screen opens a link to the agent when it has something to ask and closes it when it has its answer, so
// with none of them on the screen the plugin has no link and runs no timer. Start the agent also runs the timer
// while it waits for an agent it has started, and only while it is shown. tick() is the timer: the app calls it
// every kScreenTickMs for as long as wantsTick() is true, and all of a screen's talk with the agent happens inside
// it.
// An agent that does not answer within kScreenAnswerTicks of them is taken for not running.
//
// A screen is driven from the app's one thread. open() of a link is not called from inside a link's callback.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <string>

#include "agent_link.hpp"

namespace nbt {

const unsigned kScreenTickMs = 200;
const unsigned kScreenAnswerTicks = 25;   // 5 s
const unsigned kStartProbeTicks = 5;      // 1 s: how often Start the agent asks again after a start
const unsigned kStartWaitTicks = 150;     // 30 s: how long it asks before it says the start did not work

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
        // What was sent is not taken back: the agent forgets, and nobody is told.
        link_.close();
        forgetId_ = 0;
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

}  // namespace nbt

#endif

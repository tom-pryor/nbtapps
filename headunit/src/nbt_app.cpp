// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
//
// libNbtApp.so - NBTApp as a native iDrive "PluginApp", built on the HU Framework like the head unit's own
// SecurityMenu / ActiveSeating apps.
//
// HmiMain3D's CAppManager loads every <ro|rw>/Plugins/<Name>/<Name>.manifest at boot. The manifest names this
// library, the UI description (NbtApp.xml) and the text/image DBs. The XML declares the entry button and where it
// goes, so iDrive itself creates the menu row; pressing it opens our menu (hmiState 10). Its rows open our other
// states, and those steps are XML hmiActions:
//
//   Start the agent     hmiState 16. Says whether the agent is running; with none, its second row starts the
//                       agent's launcher (agent_start.hpp, startByHand), and the screen says when the agent is
//                       there. How a head unit with no console gets its first agent, which a phone can then have
//                       started with the head unit.
//   Pair a phone        hmiState 14. While it is shown the agent has a pairing window open, and the screen shows the
//                       window's code: six digits, as two texts of three. The user types the code into the phone
//                       app, and the screen says how that went.
//   Forget all phones   hmiState 15. Its second row tells the agent to forget every paired phone.
//   About               hmiState 12.
//
// What the screens say to the agent, and when, is in phone_screens.hpp, and how they reach it in
// agent_link.hpp: the agent's local socket, opened while one of the screens has something to ask and at no other
// time. This file is the part that needs the HU Framework: it hears when a state is shown and left, turns what a
// screen has to say into texts of the text DB, and runs the timer the screens are driven by.
//
// The HMI shows texts of the text DB only, so everything a screen can say is a text there (tools/make_db.py), and
// a model is pointed at another text to change what a row reads. The code's two halves are two of the thousand
// texts "000" to "999".
//
// create() also starts the agent's launcher, on a head unit that is to start the agent by itself: when, with
// what, and what keeps it from doing so is in agent_start.hpp. It is one line in the log each time the HMI
// starts, and a start from the Start the agent screen is one line more.
//
// The app asks the HMI to tell it when one of its states is shown and left (addHandler with type 11, visible) and
// when it has the focus (type 1), and each one that comes is a line in the log. A state that is never reported as
// shown leaves the Pair a phone screen at its first text, with no code.
// `nbtctl pair` on the head unit does what the Pair a phone screen does, with none of these.
//
// ABI:
//   pluginAppInfo = { u16 1, u16 0, name, vendor, u32 rhmiVersion 1, create, destroy }
//   create(IDispatcher&, const char* name, const Bundle& params, const Bundle& config) -> IPluginApp*
// The app object is a bmw::PluginApp built with the framework's own constructor. It gets its own copy of the
// PluginApp vtable with onInit / onError / onFinalize replaced, which is what a C++ subclass would do.
//
// Log: slog major 0x4e42 (`sloginfo -m 20034`, or `sloginfo | grep NbtApp`). A pairing code is never logged.
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <new>
#include <string>
#include <typeinfo>
#include <sys/stat.h>
#ifdef __QNX__
#include <sys/slog.h>
#include <sys/slogcodes.h>
#endif
#include <time.h>
#include "agent_start.hpp"
#include "hu_framework.hpp"
#include "phone_screens.hpp"
#include "version.hpp"

// Set by the Makefile from `git describe` (VERSION). No build date or time: the same source gives the same binary.
#ifndef NBT_APP_VERSION
#define NBT_APP_VERSION "unknown"
#endif

namespace {

// What the library says it is, in its log: the plugin's version (version.hpp) and the commit it was built from.
// tools/plugin_pkg.py reads this line out of the library before it signs a release, so its form is fixed.
const char kBuildLine[] = "NBTApp plugin " NBT_PLUGIN_VERSION " (build " NBT_APP_VERSION ")";

// Kill switch on the persistence partition (/var/opt is writable at runtime): `mkdir -p /var/opt/nbtapp &&
// touch /var/opt/nbtapp/disable` makes create() return no app, so iDrive shows no entry after the next HMI start,
// and the plugin does not start the agent.
const char* const kDisableFile = "/var/opt/nbtapp/disable";

// About page (NbtApp.xml hmiState 12): a non-selectable <list> on a Richtext raListModel holding a 1x1 table whose
// cell is a text of our text DB. The HMI wraps it to the screen width and honours "\n" in the text.
const unsigned kAboutModel = 37;             // <raListModel id="37" modelType="Richtext"/>
const unsigned kAboutText  = 3;              // tools/make_db.py TEXTS

// Pair a phone (hmiState 14): three rows, each a text model that is pointed at the text to show.
const unsigned kPairState = 14;
const unsigned kPairStatusModel = 40;        // what is going on
const unsigned kPairFirstHalfModel = 41;     // the code's first three digits,
const unsigned kPairSecondHalfModel = 42;    // and its last three
const unsigned kPairAskingText = 10;
const unsigned kPairCodeText = 11;
const unsigned kPairDoneText = 12;
const unsigned kPairWrongCodesText = 13;
const unsigned kPairRanOutText = 14;
const unsigned kNoAgentText = 15;
const unsigned kPairTooSoonText = 16;
const unsigned kPairFailedText = 17;
const unsigned kNoCodeText = 20;             // "---"
const unsigned kCodeFirstText = 1000;        // "000"; the text of the three digits n is kCodeFirstText + n

// Start the agent (hmiState 16): the first row says how things stand, the second starts it (raAction 30) where
// there is something to start, and reads "---" where there is not.
const unsigned kStartState = 16;
const unsigned kStartAction = 30;
const unsigned kStartStatusModel = 46;
const unsigned kStartRowModel = 47;
const unsigned kStartCheckingText = 50;
const unsigned kStartRunningText = 51;
const unsigned kStartNowText = 52;
const unsigned kStartStartingText = 53;
const unsigned kStartStartedText = 54;
const unsigned kStartNoLauncherText = 55;
const unsigned kStartFailedText = 56;
const unsigned kStartAgainText = 57;

// Forget all phones (hmiState 15): the second row does it (raAction 28), and reads how it went.
const unsigned kForgetState = 15;
const unsigned kForgetAction = 28;
const unsigned kForgetRowModel = 44;
const unsigned kForgetAskText = 31;
const unsigned kForgetWorkingText = 32;
const unsigned kForgetDoneText = 33;
const unsigned kForgetFailedText = 34;

// The types of a state's notifications (hu_framework.hpp, addHandler).
const unsigned kNotifyFocus = 1;
const unsigned kNotifyVisible = 11;

void alog(const char* fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
#ifdef __QNX__
    slogf(_SLOG_SETCODE(0x4e42, 1), _SLOG_INFO, "[NbtApp] %s", msg);
#else
    fprintf(stderr, "[NbtApp] %s\n", msg);
#endif
}

bool fileExists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

void* g_vtbl[2 + bmw::kPluginAppVtblSlots];  // [offset-to-top, typeinfo, slots...] copied from bmw::PluginApp's vtable
bmw::PluginApp* g_app = NULL;

void showAbout(bmw::PluginApp* app) {
    bmw::Map<unsigned, bmw::Variant> cells;
    bmw::DataTableBase table(&cells, 1);
    unsigned key = 0;
    bmw::Variant& v = cells[key];
    delete v.m_obj;                          // fresh cell: NULL; ~Map frees whatever is left
    v.m_obj = new bmw::Variant::ObjectContainer<bmw::TextID>(bmw::TextID(kAboutText));
    app->updateModel(kAboutModel, table);
}

// ---- the timer -----------------------------------------------------------------------------------------------------
// post() takes a bmw::Runnable, whose base destructor the HU Framework does not export, so a class of ours cannot
// derive from it. This is one built by hand instead, as the app's own vtable is: an object whose first word points
// at { complete destructor, deleting destructor, run }, behind the two words every vtable starts with. It is static
// and never freed, so both destructors do nothing, whether or not the framework calls one after run().
struct TickRunnable {
    void* const* vptr;
};
void tickDestroy(void*) {}
void tickRun(void*);
void* g_tickVtbl[2 + 3] = {0, 0, reinterpret_cast<void*>(&tickDestroy), reinterpret_cast<void*>(&tickDestroy),
                           reinterpret_cast<void*>(&tickRun)};
TickRunnable g_tick = {g_tickVtbl + 2};
bool g_tickQueued = false;

// ---- the start by hand --------------------------------------------------------------------------------------------
// What the Start the agent screen starts: the launcher, as agent_start.hpp starts it by hand, with no wait.
class HandStarter : public nbt::AgentStarter {
public:
    virtual nbt::LaunchResult startAgent() {
        nbt::agentstart::Outcome start =
            nbt::agentstart::startByHand(nbt::agentstart::Paths(), nbt::agentstart::headUnitEnvironment(0));
        alog("agent: %s", start.line.c_str());
        switch (start.decision) {
            case nbt::agentstart::kStarted: return nbt::kLaunched;
            case nbt::agentstart::kRunning: return nbt::kLaunchRunning;
            case nbt::agentstart::kNoLauncher: return nbt::kLaunchNoLauncher;
            default: return nbt::kLaunchFailed;
        }
    }
};

// ---- the screens ---------------------------------------------------------------------------------------------------
class Screens : public nbt::PairView, public nbt::ForgetView, public nbt::StartView {
public:
    explicit Screens(bmw::PluginApp* app)
        : app_(app), pair_(nbt::kHeadUnitSocket, this), forget_(nbt::kHeadUnitSocket, this),
          start_(nbt::kHeadUnitSocket, this, &starter_) {}

    // A state of ours is shown (or has the focus), or was left.
    void pairShown() {
        // Told twice, as visible and as focused, it is one showing: the code on the screen stays.
        if (!pair_.wantsTick() || (pair_.status() != nbt::kPairAsking && pair_.status() != nbt::kPairCode)) pair_.shown();
        arm();
    }
    void pairHidden() { pair_.hidden(); }
    void forgetShown() {
        if (forget_.status() != nbt::kForgetWorking) forget_.shown();
    }
    void forgetHidden() { forget_.hidden(); }
    void startShown() {
        start_.shown();
        arm();
    }
    void startHidden() { start_.hidden(); }
    // The app is being taken down: no link to the agent outlasts it.
    void closeLinks() {
        pair_.hidden();
        forget_.hidden();
        start_.hidden();
    }
    void forgetPressed() {
        forget_.confirmed();
        arm();
    }
    void startPressed() {
        start_.pressed();
        arm();
    }

    void tick() {
        pair_.tick();
        forget_.tick();
        start_.tick();
        arm();
    }

    // PairView
    virtual void pairShow(nbt::PairStatus status, const std::string& code) {
        unsigned text = kPairFailedText;
        switch (status) {
            case nbt::kPairAsking: text = kPairAskingText; break;
            case nbt::kPairCode: text = kPairCodeText; break;
            case nbt::kPairDone: text = kPairDoneText; break;
            case nbt::kPairWrongCodes: text = kPairWrongCodesText; break;
            case nbt::kPairRanOut: text = kPairRanOutText; break;
            case nbt::kPairTooSoon: text = kPairTooSoonText; break;
            case nbt::kPairNoAgent: text = kNoAgentText; break;
            case nbt::kPairFailed: text = kPairFailedText; break;
        }
        unsigned first = kNoCodeText, second = kNoCodeText;
        if (status == nbt::kPairCode && code.size() == 6) {
            first = kCodeFirstText + (unsigned)atoi(code.substr(0, 3).c_str());
            second = kCodeFirstText + (unsigned)atoi(code.substr(3, 3).c_str());
        }
        app_->updateModel(kPairStatusModel, bmw::TextID(text));
        app_->updateModel(kPairFirstHalfModel, bmw::TextID(first));
        app_->updateModel(kPairSecondHalfModel, bmw::TextID(second));
        alog("pair a phone: status %d, text %u%s", (int)status, text, status == nbt::kPairCode ? ", a code is shown" : "");
    }

    // ForgetView
    virtual void forgetShow(nbt::ForgetStatus status) {
        unsigned text = kForgetFailedText;
        switch (status) {
            case nbt::kForgetAsk: text = kForgetAskText; break;
            case nbt::kForgetWorking: text = kForgetWorkingText; break;
            case nbt::kForgetDone: text = kForgetDoneText; break;
            case nbt::kForgetNoAgent: text = kNoAgentText; break;
            case nbt::kForgetFailed: text = kForgetFailedText; break;
        }
        app_->updateModel(kForgetRowModel, bmw::TextID(text));
        alog("forget all phones: status %d, text %u", (int)status, text);
    }

    // StartView
    virtual void startShow(nbt::StartStatus status) {
        unsigned text = kStartFailedText, row = kNoCodeText;
        switch (status) {
            case nbt::kStartChecking: text = kStartCheckingText; break;
            case nbt::kStartRunning: text = kStartRunningText; break;
            case nbt::kStartAsk:
                text = kNoAgentText;
                row = kStartNowText;
                break;
            case nbt::kStartStarting: text = kStartStartingText; break;
            case nbt::kStartStarted: text = kStartStartedText; break;
            case nbt::kStartNoLauncher: text = kStartNoLauncherText; break;
            case nbt::kStartFailed:
                text = kStartFailedText;
                row = kStartAgainText;
                break;
        }
        app_->updateModel(kStartStatusModel, bmw::TextID(text));
        app_->updateModel(kStartRowModel, bmw::TextID(row));
        alog("start the agent: status %d, texts %u and %u", (int)status, text, row);
    }

private:
    // Keeps the timer running for as long as a screen has a link to the agent, and not a tick longer.
    void arm() {
        if (g_tickQueued || !(pair_.wantsTick() || forget_.wantsTick() || start_.wantsTick())) return;
        g_tickQueued = true;
        app_->post(reinterpret_cast<bmw::Runnable*>(&g_tick), nbt::kScreenTickMs);
    }

    bmw::PluginApp* app_;
    nbt::PairScreen pair_;
    nbt::ForgetScreen forget_;
    HandStarter starter_;   // before start_, which is handed it
    nbt::StartScreen start_;
};

Screens* g_screens = NULL;

// Nothing thrown in our code gets back into the HMI's.
#define NBT_GUARDED(what, call)                              \
    do {                                                     \
        try {                                                \
            if (g_screens) { call; }                         \
        } catch (...) { alog("%s: failed", what); }          \
    } while (0)

void tickRun(void*) {
    g_tickQueued = false;
    NBT_GUARDED("tick", g_screens->tick());
}

bool readFlag(const bmw::Parcel& parcel) {
    bool value = false;
    parcel.readBool(value);
    return value;
}

bool onPairVisible(void*, const bmw::Parcel& parcel) {
    bool visible = readFlag(parcel);
    alog("state %u: visible=%d", kPairState, (int)visible);
    if (visible) NBT_GUARDED("pair shown", g_screens->pairShown());
    else NBT_GUARDED("pair hidden", g_screens->pairHidden());
    return true;
}

bool onPairFocus(void*, const bmw::Parcel& parcel) {
    bool focus = readFlag(parcel);
    alog("state %u: focus=%d", kPairState, (int)focus);
    // Losing the focus is not leaving: only `visible` closes the window.
    if (focus) NBT_GUARDED("pair focus", g_screens->pairShown());
    return true;
}

bool onForgetVisible(void*, const bmw::Parcel& parcel) {
    bool visible = readFlag(parcel);
    alog("state %u: visible=%d", kForgetState, (int)visible);
    if (visible) NBT_GUARDED("forget shown", g_screens->forgetShown());
    else NBT_GUARDED("forget hidden", g_screens->forgetHidden());
    return true;
}

bool onStartVisible(void*, const bmw::Parcel& parcel) {
    bool visible = readFlag(parcel);
    alog("state %u: visible=%d", kStartState, (int)visible);
    if (visible) NBT_GUARDED("start shown", g_screens->startShown());
    else NBT_GUARDED("start hidden", g_screens->startHidden());
    return true;
}

bool onStartPressed(void*, const bmw::Parcel&) {
    alog("action %u: start the agent", kStartAction);
    NBT_GUARDED("start pressed", g_screens->startPressed());
    return true;
}

bool onForgetPressed(void*, const bmw::Parcel&) {
    alog("action %u: forget all phones", kForgetAction);
    NBT_GUARDED("forget pressed", g_screens->forgetPressed());
    return true;
}

// A delegate for one of the handlers above. Its object is the app, which no handler here reads: the framework
// calls a delegate only when its object is set as well as its function (hu_framework.hpp, Delegate), so one with
// no object is registered and never called. addHandler copies the two words, so the delegate need not outlast it.
bmw::ActionDelegate handler(bmw::PluginApp* self, bool (*stub)(void*, const bmw::Parcel&)) {
    bmw::ActionDelegate delegate = {self, stub};
    return delegate;
}

bool appOnInit(bmw::PluginApp* self) {
    bool ok = self->onInit();
    showAbout(self);
    self->addHandler(kPairState, kNotifyVisible, handler(self, &onPairVisible));
    self->addHandler(kPairState, kNotifyFocus, handler(self, &onPairFocus));
    self->addHandler(kForgetState, kNotifyVisible, handler(self, &onForgetVisible));
    self->addHandler(kForgetAction, handler(self, &onForgetPressed));
    self->addHandler(kStartState, kNotifyVisible, handler(self, &onStartVisible));
    self->addHandler(kStartAction, handler(self, &onStartPressed));
    alog("onInit: base onInit -> %d, About model %u <- text %u, handlers for states %u, %u and %u and actions %u "
         "and %u", (int)ok, kAboutModel, kAboutText, kPairState, kForgetState, kStartState, kForgetAction, kStartAction);
    return ok;
}

void appOnError(bmw::PluginApp* /*self*/, unsigned code, const bmw::BasicString<char>& text) {
    const char* s = text.raw();
    alog("onError %u: %s", code, s ? s : "(null)");
}

void appOnFinalize(bmw::PluginApp* self) {
    alog("onFinalize");
    NBT_GUARDED("finalize", g_screens->closeLinks());
    self->onFinalize();
}

bmw::PluginApp* createApp(bmw::IDispatcher& dispatcher, const char* name, const bmw::Bundle& params,
                          const bmw::Bundle& config) {
    alog("create(name=%s) %s", name ? name : "(null)", kBuildLine);
    if (fileExists(kDisableFile)) { alog("disabled by %s", kDisableFile); return NULL; }
    if (g_app) { alog("create: already created"); return NULL; }

    void* mem = ::operator new(bmw::kPluginAppStorage);
    memset(mem, 0, bmw::kPluginAppStorage);
    bmw::PluginApp* app = new (mem) bmw::PluginApp(dispatcher, bmw::BasicString<char>(name ? name : "NbtApp"), 1, 0,
                                                   bmw::RHMI_INTERFACE_VERSION_1, params, config);

    // Own vtable: identical to bmw::PluginApp's, with our onInit / onError / onFinalize.
    void** base = *reinterpret_cast<void***>(app);
    memcpy(g_vtbl, base - 2, sizeof(g_vtbl));
    g_vtbl[2 + bmw::kSlotOnInit]     = reinterpret_cast<void*>(&appOnInit);
    g_vtbl[2 + bmw::kSlotOnError]    = reinterpret_cast<void*>(&appOnError);
    g_vtbl[2 + bmw::kSlotOnFinalize] = reinterpret_cast<void*>(&appOnFinalize);
    *reinterpret_cast<void***>(app) = g_vtbl + 2;

    // The timer's object says it is a bmw::Runnable to whoever asks its type.
    g_tickVtbl[1] = const_cast<void*>(static_cast<const void*>(&typeid(bmw::Runnable)));
    try {
        g_screens = new Screens(app);
    } catch (...) {
        g_screens = NULL;
        alog("create: the screens could not be made; the rest goes on without them");
    }

    g_app = app;
    alog("create: app %p (PluginApp vtable %p)", (void*)app, (void*)base);

    // After the app is made: whatever becomes of this, the menu entry is there. Behind the kill switch above, so
    // that file also keeps a head unit from starting the agent.
    try {
        nbt::agentstart::Outcome start =
            nbt::agentstart::startWithHeadUnit(nbt::agentstart::Paths(), (int64_t)time(NULL),
                                               nbt::agentstart::headUnitEnvironment(nbt::agentstart::kDelayS));
        alog("agent: %s", start.line.c_str());
    } catch (...) {
        alog("agent: the start with the head unit failed");
    }
    return app;
}

void destroyApp(void* app) {
    alog("destroy(%p)", app);
    if (!app) return;
    delete g_screens;
    g_screens = NULL;
    void** vt = *reinterpret_cast<void***>(app);
    reinterpret_cast<void (*)(void*)>(vt[1])(app);   // deleting destructor (PluginApp::~PluginApp + operator delete)
    if (app == g_app) g_app = NULL;
}

__attribute__((constructor)) void libraryLoaded() {
    alog("library loaded into pid %d, %s", (int)getpid(), kBuildLine);
}

} // namespace

// ---- entry point looked up by ManagedPluginApp::init (dlsym "pluginAppInfo") -------------------------------------
extern "C" {
struct NbtPluginAppInfo {
    uint16_t versionMajor;
    uint16_t versionMinor;
    const char* name;
    const char* vendor;
    uint32_t rhmiInterfaceVersion;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
__attribute__((visibility("default"))) NbtPluginAppInfo pluginAppInfo = {
    1, 0, "NBTApp", "NBTApp", 1, &createApp, &destroyApp
};
}

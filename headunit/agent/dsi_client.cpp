// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#include "dsi_client.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

namespace nbt {

namespace {

typedef void (*FnConnCtor)(void*);
typedef int  (*FnConnAdd)(void*, void*);
typedef void (*FnConnStart)(void*);
typedef void (*FnCliCtor)(void*, const char*, const char*, int, int);
typedef void (*FnSetNotif)(void*, unsigned);
typedef void (*FnStreamCtor)(void*, void*, void*, int, unsigned, unsigned, unsigned, const void*, const void*, int);
typedef void (*FnStreamWrite)(void*, const void*, unsigned);
typedef int  (*FnStreamSend)(void*, int);
typedef void (*FnStreamDtor)(void*);

struct Syms {
    FnConnCtor connCtor;
    FnConnAdd connAdd;
    FnConnStart connStart;
    FnCliCtor cliCtor;
    FnSetNotif setNotif;
    FnStreamCtor streamCtor;
    FnStreamWrite streamWrite;
    FnStreamSend streamSend;
    FnStreamDtor streamDtor;
    void* vtable;
};

Syms g_s;
pthread_mutex_t g_symMutex = PTHREAD_MUTEX_INITIALIZER;

// The library calls the replaced virtuals with its CDSIClient object as `this`; this table maps it back to ours.
enum { kMaxClients = 4 };
struct Slot { void* cli; DsiClient* self; };
Slot g_slots[kMaxClients];
int g_nslots = 0;               // only grows; an entry is written before CDSIConnection::add

DsiClient* lookup(void* cli) {
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].cli == cli) return g_slots[i].self;
    return NULL;
}

// Sizes of the library's objects (from its constructors), with slack.
const size_t kConnSize = 0x8c + 64;
const size_t kClientSize = 0x14c + 64;
const size_t kStreamSize = 0xbc + 64;

} // namespace

DsiClient::DsiClient()
    : lib_(NULL), conn_(NULL), cli_(NULL), attached_(false), verbose_(false), attachedCb_(NULL), dataCb_(NULL),
      detachedCb_(NULL), user_(NULL), log_(NULL), logUser_(NULL) {}

void DsiClient::say(const char* fmt, ...) const {
    char line[512] = "[dsi] ";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + 6, sizeof(line) - 6, fmt, ap);
    va_end(ap);
    if (log_) {
        log_(line, logUser_);
    } else {
        printf("%s\n", line);
        fflush(stdout);
    }
}

void* DsiClient::need(const char* name) const {
    void* p = dlsym(lib_, name);
    if (!p) say("symbol missing in the DSI library: %s", name);
    return p;
}

void DsiClient::setCallbacks(AttachedCb attached, DataCb data, DetachedCb detached, void* user) {
    attachedCb_ = attached;
    dataCb_ = data;
    detachedCb_ = detached;
    user_ = user;
}

bool DsiClient::open(const char* iface, const char* role, int major, int minor) {
    if (cli_) { say("open: this client is already open"); return false; }
    // The object sizes, field offsets and vtable slots below are those of the head unit's library, which is 32-bit.
    if (sizeof(void*) != 4) { say("open: this client works in a 32-bit program only"); return false; }
    if (g_nslots >= kMaxClients) { say("open: too many clients (max %d)", (int)kMaxClients); return false; }

    const char* const names[] = { "/opt/sys/lib/libsys_dsi_colibry.cpp-ne.so.8",
                                  "/fs/sda0/opt/sys/lib/libsys_dsi_colibry.cpp-ne.so.8",
                                  "libsys_dsi_colibry.cpp-ne.so.8", NULL };
    for (int i = 0; names[i] && !lib_; ++i) {
        lib_ = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
        if (verbose_) say("dlopen(%s): %s", names[i], lib_ ? "ok" : dlerror());
    }
    if (!lib_) {
        say("the DSI library %s could not be loaded: %s", names[2], dlerror());
        return false;
    }

    pthread_mutex_lock(&g_symMutex);
    if (!g_s.vtable) {   // resolved once per process
        memset(&g_s, 0, sizeof(g_s));
        g_s.connCtor    = (FnConnCtor)   need("_ZN14CDSIConnectionC1Ev");
        g_s.connAdd     = (FnConnAdd)    need("_ZN14CDSIConnection3addER10CDSIClient");
        g_s.connStart   = (FnConnStart)  need("_ZN14CDSIConnection5startEv");
        g_s.cliCtor     = (FnCliCtor)    need("_ZN10CDSIClientC1EPKcS1_ii");
        g_s.setNotif    = (FnSetNotif)   need("_ZN10CDSIClient15setNotificationEj");
        g_s.streamCtor  = (FnStreamCtor) need("_ZN11CDSIOStreamC1ER11CDSIChannelP13CDSITCPSocketiN3DSI11RequestTypeENS4_7CommandEjRK8SPartyIDS9_i");
        g_s.streamWrite = (FnStreamWrite)need("_ZN11CDSIOStream5writeEPKvj");
        g_s.streamSend  = (FnStreamSend) need("_ZN11CDSIOStream4sendEi");
        g_s.streamDtor  = (FnStreamDtor) need("_ZN11CDSIOStreamD1Ev");
        g_s.vtable      = need("_ZTV10CDSIClient");
        if (!g_s.connCtor || !g_s.connAdd || !g_s.connStart || !g_s.cliCtor || !g_s.setNotif || !g_s.streamCtor ||
            !g_s.streamWrite || !g_s.streamSend || !g_s.streamDtor || !g_s.vtable) {
            say("this head unit's DSI library lacks symbols this client needs: not connecting");
            memset(&g_s, 0, sizeof(g_s));
            pthread_mutex_unlock(&g_symMutex);
            return false;
        }
    }
    pthread_mutex_unlock(&g_symMutex);

    conn_ = calloc(1, kConnSize);
    cli_  = static_cast<char*>(calloc(1, kClientSize));
    if (!conn_ || !cli_) return false;

    g_s.connCtor(conn_);
    g_s.cliCtor(cli_, iface, role, major, minor);

    // CDSIClient is abstract: give the object a copy of its vtable (vptr = _ZTV + 8) with three slots replaced.
    // 32 slots are copied so that a virtual beyond the ones known here still resolves to the library's own entry.
    void** vt = static_cast<void**>(calloc(32, sizeof(void*)));
    if (!vt) return false;
    memcpy(vt, static_cast<char*>(g_s.vtable) + 8, 32 * sizeof(void*));
    vt[3] = reinterpret_cast<void*>(&DsiClient::onAttachedTramp);   // +0x0c attached
    vt[4] = reinterpret_cast<void*>(&DsiClient::onDetachedTramp);   // +0x10 detached
    vt[7] = reinterpret_cast<void*>(&DsiClient::onDataTramp);       // +0x1c handleDataResponse
    *reinterpret_cast<void***>(cli_) = vt;

    pthread_mutex_lock(&g_symMutex);
    g_slots[g_nslots].cli = cli_;
    g_slots[g_nslots].self = this;
    ++g_nslots;
    pthread_mutex_unlock(&g_symMutex);

    int added = g_s.connAdd(conn_, cli_);
    if (verbose_) say("client %s.%s %d.%d added to its connection (%d)", role, iface, major, minor, added);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    bool started = pthread_create(&t, &at, &DsiClient::threadMain, this) == 0;
    pthread_attr_destroy(&at);
    if (!started) say("could not start the connection thread");
    return started;
}

void* DsiClient::threadMain(void* arg) {
    DsiClient* c = static_cast<DsiClient*>(arg);
    g_s.connStart(c->conn_);   // the library's loop: blocks for the life of the connection
    c->say("the connection loop ended");
    return NULL;
}

void DsiClient::onAttachedTramp(void* self) {
    DsiClient* c = lookup(self);
    if (!c) return;
    c->attached_ = true;
    if (c->attachedCb_) c->attachedCb_(c, c->user_);
}

void DsiClient::onDetachedTramp(void* self) {
    DsiClient* c = lookup(self);
    if (!c) return;
    c->attached_ = false;
    if (c->detachedCb_) c->detachedCb_(c, c->user_);
}

void DsiClient::onDataTramp(void* self, void* istream) {
    DsiClient* c = lookup(self);
    if (!c || !istream) return;
    // CDSIIStream fields: +0x08 payload length, +0x10 payload pointer, +0x44 type, +0x48 id, +0x4c sequence.
    const char* s = static_cast<const char*>(istream);
    uint32_t type = *reinterpret_cast<const uint32_t*>(s + 0x44);
    uint32_t id   = *reinterpret_cast<const uint32_t*>(s + 0x48);
    uint32_t seq  = *reinterpret_cast<const uint32_t*>(s + 0x4c);
    const uint8_t* pl = *reinterpret_cast<const uint8_t* const*>(s + 0x10);
    uint32_t len  = *reinterpret_cast<const uint32_t*>(s + 0x08);
    if (c->verbose_) {
        char hex[3 * 48 + 1] = "";
        for (uint32_t i = 0; pl && i < len && i < 48; ++i) snprintf(hex + 3 * i, 4, " %02x", pl[i]);
        c->say("RX type=0x%x id=0x%x seq=%u len=%u:%s", type, id, seq, len, hex);
    }
    if (c->dataCb_) c->dataCb_(c, type, id, seq, pl, len, c->user_);
}

bool DsiClient::setNotification(uint32_t informId) {
    if (!cli_ || !attached_) return false;
    g_s.setNotif(cli_, informId);
    if (verbose_) say("setNotification(0x%x)", informId);
    return true;
}

bool DsiClient::sendRequest(uint32_t requestId, const std::vector<uint8_t>& payload) {
    if (!cli_ || !attached_) return false;
    // CDSIOStream holds 64-bit members: its storage must be 8-byte aligned (LDRD/STRD fault otherwise on ARM).
    uint64_t storage[kStreamSize / 8 + 1];
    memset(storage, 0, sizeof(storage));
    char* stream = reinterpret_cast<char*>(storage);
    // CDSIClient fields: +0x110 client party, +0x118 server party, +0x130 CDSIChannel, +0x138 CDSITCPSocket,
    // +0x140 ipc fd. Request type 0x100, command 7.
    void* chan = cli_ + 0x130;
    void* tcp  = cli_ + 0x138;
    int ipcFd  = *reinterpret_cast<int*>(cli_ + 0x140);
    g_s.streamCtor(stream, chan, tcp, ipcFd, 0x100u, 7u, requestId, cli_ + 0x110, cli_ + 0x118, 0);
    if (!payload.empty()) g_s.streamWrite(stream, &payload[0], (unsigned)payload.size());
    int rc = g_s.streamSend(stream, -1);
    g_s.streamDtor(stream);
    if (verbose_) say("TX request id=0x%x, %u bytes, send -> %d", requestId, (unsigned)payload.size(), rc);
    return true;
}

} // namespace nbt

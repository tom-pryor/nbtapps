// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The gate in front of /dev/btspp0 (see spp_gate.hpp): a resource manager with one thread that answers opens.
#include "spp_gate.hpp"

#ifdef __QNXNTO__

#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include <fcntl.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>

namespace nbt {
namespace btspp {

struct SppGate::Impl {
    enum { kSeen = 16 };

    // What the resource manager library hands back to the open handler: the attributes it wants first, then us.
    struct Handle {
        iofunc_attr_t attr;
        Impl* impl;
    };

    GateMode mode;
    std::string refuse;
    OpenCb cb;
    void* user;

    bool attached;
    Handle handle;
    dispatch_t* dpp;
    dispatch_context_t* ctp;
    int id;
    resmgr_connect_funcs_t connect;
    resmgr_io_funcs_t io;
    pthread_t thread;
    bool hasThread;

    pthread_mutex_t mu;   // running, held, holds, seen, seenAt
    bool running;
    bool held;
    std::vector<std::string> holds;
    long seen[kSeen];
    unsigned seenAt;

    Impl() : mode(kGateLog), cb(NULL), user(NULL), attached(false), dpp(NULL), ctp(NULL), id(-1), hasThread(false),
             running(false), held(false), seenAt(0) {
        pthread_mutex_init(&mu, NULL);
        for (int i = 0; i < kSeen; ++i) seen[i] = 0;
    }
    ~Impl() { pthread_mutex_destroy(&mu); }

    bool isRunning() {
        pthread_mutex_lock(&mu);
        bool r = running;
        pthread_mutex_unlock(&mu);
        return r;
    }
    void setRunning(bool r) {
        pthread_mutex_lock(&mu);
        running = r;
        pthread_mutex_unlock(&mu);
    }
    // Whether the hold refuses this open (gateHolds).
    bool holdsNow(const std::string& program, GateOpen::Kind kind) {
        pthread_mutex_lock(&mu);
        bool h = gateHolds(mode, program, holds, held, kind);
        pthread_mutex_unlock(&mu);
        return h;
    }
    void setHeld(bool h) {
        pthread_mutex_lock(&mu);
        held = h;
        pthread_mutex_unlock(&mu);
    }
    void setHolds(const std::vector<std::string>& programs) {
        pthread_mutex_lock(&mu);
        holds = programs;
        pthread_mutex_unlock(&mu);
    }
    void remember(long pid) {
        pthread_mutex_lock(&mu);
        seen[seenAt] = pid;
        seenAt = (seenAt + 1) % kSeen;
        pthread_mutex_unlock(&mu);
    }

    static int onOpen(resmgr_context_t* ctp, io_open_t* msg, RESMGR_HANDLE_T* handle, void* extra);
    static void* threadMain(void* arg);
};

namespace {

std::string failed(const char* what) { return std::string(what) + ": " + strerror(errno); }

}  // namespace

// Every open of the path by another process arrives here: stat() and access() too, which start with one.
int SppGate::Impl::onOpen(resmgr_context_t* ctp, io_open_t* msg, RESMGR_HANDLE_T* handle, void*) {
    Impl* g = reinterpret_cast<Handle*>(handle)->impl;
    GateOpen o;
    o.pid = (long)ctp->info.pid;
    o.program = programName(nbt::exePathOf(o.pid).c_str());   // programs.hpp
    o.kind = msg->connect.subtype == _IO_CONNECT_COMBINE_CLOSE ? GateOpen::kLook
           : (msg->connect.subtype == _IO_CONNECT_COMBINE ? GateOpen::kCombined : GateOpen::kOpen);
    o.read = (msg->connect.ioflag & _IO_FLAG_RD) != 0;
    o.write = (msg->connect.ioflag & _IO_FLAG_WR) != 0;
    o.answer = gateAnswer(g->mode, o.program, g->refuse);
    if (o.answer != EACCES && g->holdsNow(o.program, o.kind)) {
        o.answer = EACCES;
        o.held = true;
    }
    g->remember(o.pid);
    if (g->cb) g->cb(o, g->user);
    return o.answer;
}

void* SppGate::Impl::threadMain(void* arg) {
    Impl* g = static_cast<Impl*>(arg);
    // The wait for a message ends twice a second with ETIMEDOUT, so that detach() is noticed.
    struct timespec tick = { 0, 500000000L };
    dispatch_timeout(g->dpp, &tick);
    while (g->isRunning()) {
        dispatch_context_t* r = dispatch_block(g->ctp);
        if (r) {
            g->ctp = r;
            dispatch_handler(r);
        } else if (errno != ETIMEDOUT && errno != EINTR) {
            break;
        }
    }
    return NULL;
}

SppGate::SppGate() : impl_(new Impl()) {}

SppGate::~SppGate() {
    detach();
    delete impl_;
}

void SppGate::configure(GateMode mode, const std::string& refuse, OpenCb cb, void* user) {
    impl_->mode = mode;
    impl_->refuse = refuse;
    impl_->cb = cb;
    impl_->user = user;
}

void SppGate::setHolds(const std::vector<std::string>& programs) { impl_->setHolds(programs); }

void SppGate::setHold(const std::string& program) {
    impl_->setHolds(program.empty() ? std::vector<std::string>() : std::vector<std::string>(1, program));
}

void SppGate::hold(bool on) { impl_->setHeld(on); }

bool SppGate::attached() const { return impl_->attached; }

bool SppGate::attach(const char* path, std::string* error) {
    Impl* g = impl_;
    if (g->attached) return true;
    g->dpp = dispatch_create();
    if (!g->dpp) {
        *error = failed("dispatch_create");
        return false;
    }
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g->connect, _RESMGR_IO_NFUNCS, &g->io);
    // Opens only. Every other request about the path gets "not handled here" from the library, which sends the
    // asker on to the next server just as ENOENT does.
    g->connect.open = &Impl::onOpen;
    g->connect.unlink = NULL;
    g->connect.rename = NULL;
    g->connect.mknod = NULL;
    g->connect.readlink = NULL;
    g->connect.link = NULL;
    g->connect.mount = NULL;
    iofunc_attr_init(&g->handle.attr, S_IFCHR | 0666, NULL, NULL);
    g->handle.impl = g;
    resmgr_attr_t ra;
    memset(&ra, 0, sizeof(ra));
    ra.nparts_max = 1;
    ra.msg_max_size = 2048;
    // _RESMGR_FLAG_BEFORE: ahead of the other servers at this path, the stack's node among them, which is
    // attached later, each time a link comes up. No _RESMGR_FLAG_SELF: this process's own opens of the path do
    // not come here but go to the stack's node.
    g->id = resmgr_attach(g->dpp, &ra, path, _FTYPE_ANY, _RESMGR_FLAG_BEFORE, &g->connect, &g->io,
                          reinterpret_cast<RESMGR_HANDLE_T*>(&g->handle));
    if (g->id == -1) {
        *error = failed("resmgr_attach");
        dispatch_destroy(g->dpp);
        g->dpp = NULL;
        return false;
    }
    g->ctp = dispatch_context_alloc(g->dpp);
    if (!g->ctp) *error = failed("dispatch_context_alloc");
    g->setRunning(true);
    g->hasThread = g->ctp && pthread_create(&g->thread, NULL, &Impl::threadMain, g) == 0;
    if (g->ctp && !g->hasThread) *error = "the gate's thread could not be started";
    g->attached = true;   // for detach(), which takes apart whatever is there
    if (!g->hasThread) {
        detach();
        return false;
    }
    return true;
}

void SppGate::detach() {
    Impl* g = impl_;
    if (!g->attached) return;
    if (g->id != -1) resmgr_detach(g->dpp, g->id, _RESMGR_DETACH_ALL);
    g->id = -1;
    if (g->hasThread) {
        g->setRunning(false);
        pthread_join(g->thread, NULL);
        g->hasThread = false;
    }
    if (g->ctp) dispatch_context_free(g->ctp);
    g->ctp = NULL;
    if (g->dpp) dispatch_destroy(g->dpp);
    g->dpp = NULL;
    g->attached = false;
}

bool SppGate::sawPid(long pid) {
    Impl* g = impl_;
    bool saw = false;
    pthread_mutex_lock(&g->mu);
    for (int i = 0; i < Impl::kSeen; ++i) saw = saw || g->seen[i] == pid;
    pthread_mutex_unlock(&g->mu);
    return saw;
}

}  // namespace btspp
}  // namespace nbt

#else   // not the head unit's system: there is no path space to attach to

namespace nbt {
namespace btspp {

struct SppGate::Impl {};

SppGate::SppGate() : impl_(NULL) {}
SppGate::~SppGate() {}
void SppGate::configure(GateMode, const std::string&, OpenCb, void*) {}
void SppGate::setHolds(const std::vector<std::string>&) {}
void SppGate::setHold(const std::string&) {}
void SppGate::hold(bool) {}
bool SppGate::attach(const char*, std::string* error) {
    *error = "this build is not for a head unit, and only the head unit's system can put a gate in front of a path";
    return false;
}
void SppGate::detach() {}
bool SppGate::attached() const { return false; }
bool SppGate::sawPid(long) { return false; }

}  // namespace btspp
}  // namespace nbt

#endif

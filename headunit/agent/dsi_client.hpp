// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_DSI_CLIENT_HPP
#define NBT_AGENT_DSI_CLIENT_HPP

// Minimal DSI client: the head unit's service-to-service IPC, spoken through its own library
// libsys_dsi_colibry (loaded with dlopen; there are no headers, so objects are raw storage and the calls go through
// the library's exported C++ symbols). QNX only.
//
// Threading: the library runs one connection thread per client. The attached / data callbacks arrive on that thread.
// sendRequest() and setNotification() are safe from inside those callbacks; a send from another thread works as long
// as no callback of this client is sending at the same moment, which the caller has to ensure (one lock around every
// send).

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace nbt {

class DsiClient {
public:
    typedef void (*AttachedCb)(DsiClient* c, void* user);
    typedef void (*DataCb)(DsiClient* c, uint32_t type, uint32_t id, uint32_t seq, const uint8_t* payload,
                           uint32_t len, void* user);
    typedef void (*DetachedCb)(DsiClient* c, void* user);

    DsiClient();

    // Before open().
    void setCallbacks(AttachedCb attached, DataCb data, DetachedCb detached, void* user);
    // verbose: log every received message (hex) and every request sent. A received message can hold a phone's
    // name and address.
    void setVerbose(bool v) { verbose_ = v; }
    // Where this client's own lines go ("[dsi] ..."), each without a line end, from whichever thread has one to
    // say. Without it they go to standard output.
    typedef void (*LogFn)(const char* line, void* user);
    void setLog(LogFn log, void* user) { log_ = log; logUser_ = user; }

    // The server is "<role>.<iface>" at the service broker, e.g. open("SGBluetoothGap", "BSS_GAP", 35, 0).
    // Returns once the connection thread runs; `attached` follows when the server answers.
    bool open(const char* iface, const char* role, int major, int minor);

    bool attached() const { return attached_; }
    bool sendRequest(uint32_t requestId, const std::vector<uint8_t>& payload);
    bool setNotification(uint32_t informId);   // ask for one inform (event) of the server

    // Trampolines the library calls in place of CDSIClient's virtuals (public for the C-style casts only).
    static void onAttachedTramp(void* self);
    static void onDetachedTramp(void* self);
    static void onDataTramp(void* self, void* istream);

private:
    static void* threadMain(void* arg);
    void say(const char* fmt, ...) const __attribute__((format(printf, 2, 3)));
    void* need(const char* name) const;

    void* lib_;
    void* conn_;
    char* cli_;
    volatile bool attached_;
    bool verbose_;
    AttachedCb attachedCb_;
    DataCb dataCb_;
    DetachedCb detachedCb_;
    void* user_;
    LogFn log_;
    void* logUser_;
};

} // namespace nbt

#endif

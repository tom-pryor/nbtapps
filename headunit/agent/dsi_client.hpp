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
#include <string>
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
    // Returns once the connection thread runs; `attached` follows when the server answers. False, said in the log,
    // if it cannot: a caller may try again, and the same reason is not said twice in a row. Unless openFinal().
    bool open(const char* iface, const char* role, int major, int minor);
    // After open() has returned false: it failed once the client object had been made (no memory for its vtable,
    // the connection thread not started), and is not made resumable. Every later open() fails the same way, without
    // a word: the program has to be started again.
    bool openFinal() const { return final_; }

    bool attached() const { return attached_; }
    // Both false when the server is not attached or the message could not be sent; a failed send is logged.
    bool sendRequest(uint32_t requestId, const std::vector<uint8_t>& payload);
    // Asks for one inform (event) of the server. Never a reply's id: that brings every other client's replies of
    // that id too (btspp::isResponseId).
    bool setNotification(uint32_t informId);

    // Trampolines the library calls in place of CDSIClient's virtuals (public for the C-style casts only).
    static void onAttachedTramp(void* self);
    static void onDetachedTramp(void* self);
    static void onDataTramp(void* self, void* istream);

private:
    static void* threadMain(void* arg);
    int send(uint32_t requestType, uint32_t id, const std::vector<uint8_t>& payload);
    bool failOpen(const char* why, const char* detail);
    bool failFinal(const char* why);
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
    std::string lastFailure_;   // why open() last did not work
    bool symbolsTried_;         // the library's symbols were looked for, and some were missing
    bool final_;                // see openFinal()
};

} // namespace nbt

#endif

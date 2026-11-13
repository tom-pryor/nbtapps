// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// nbtctl: a command-line controller for the agent, over its local socket. It speaks the protocol a phone speaks
// (role "cli"), and with --from it is also a package source: it offers the .nbtpkg files of a folder and serves
// them when the agent asks with blob.fetch. That makes it the stand-in for a phone in the tests, and for a USB
// stick until the plugin UI can start an install.
//
//   nbtctl [--socket PATH] COMMAND ...
//
// The socket is --socket's, or else $NBT_AGENT_SOCKET's. The build for the head unit then falls back on
// /tmp/nbtagent.sock, where the agent of a head unit listens; the host's build has no such default.
//
//   info                                   the head unit's profile
//   list                                   installed packages
//   sources                                the package keys the agent trusts
//   plan    --from DIR [ID[@VERSION]...] [--remove ID...] [--delete-data]
//                                          show what a job would do; changes nothing
//   install --from DIR ID[@VERSION]... [--remove ID...] [--delete-data]
//                                          plan, start, serve the archives, and wait for the result
//   remove  [--delete-data] ID...          uninstall
//   resume  --from DIR                     start a paused job again and serve it to the end
//   status  [--events]                     the running or last job; with --events, its log too
//   cancel                                 stop the job that is fetching or paused
//   restart hmi|reboot
//   pair                                   open the pairing window: prints the code for the phone app, waits
//                                          until the window has closed, and says how. Stopping nbtctl closes it
//   phones                                 the paired phones: id, the date it was paired (UTC), name
//   forget  PHONEID | --all                end one phone's pairing, or every phone's
//   autostart [on|off]                     whether the agent is started with the head unit, and whether it can
//                                          be here; with on or off, switch it
//   logs    ID                             the log files of an installed package (org.nbtapp.agent: the agent's
//                                          own) a controller may pull: size, time (UTC) and path, newest first
//   pull    ID PATH --out FILE [--offset N] [--length N] [--none]
//                                          one of them, into FILE: deflated on the link, or with --none as it is
//   call METHOD [JSON]                     any request, the reply printed as it came
//
// `autostart` prints three lines: "autostart on" or "autostart off"; "available yes", or "available no" with the
// reason and, where it is the iDrive plugin that is wanting, its package and the versions that would do; and
// "starts N", the starts with the head unit on record that no agent outlasted by five minutes.
//
// `pull` prints one line when the file is whole: "pulled N bytes, M on the link". A stream that ends short, does
// not inflate, or that the agent ends with files.error leaves FILE as far as it came, says why, and exits with 1.
//
// `pair` prints "code NNNNNN" as its first line, then one line when the window has closed: "paired", or
// "not paired: " and the reason (wrong-code, expired, replaced). Its exit status is 0 only for "paired".
//
// With no version, a target means the highest version in the folder. Every package in the folder is offered, so a
// dependency is found there too.
//
// Exit status: 0 done; 1 the agent refused, or the job did not succeed; 2 usage; 3 no agent, or the link broke.
//
// For the tests, with install and resume:  --cancel-after N   stop the job from this side after N archive bytes
//                                          --drop-after N     end the process, uncleanly, after N archive bytes
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "frame.hpp"
#include "fsutil.hpp"
#include "hex.hpp"
#include "inflate.hpp"
#include "json.hpp"
#include "json_text.hpp"
#include "json_writer.hpp"
#include "package.hpp"
#include "peer.hpp"
#include "semver.hpp"

using namespace nbt;

namespace {

const size_t kQueueLow = 256u << 10;   // DATA frames are queued only while less than this waits to be written

// A package in the --from folder.
struct Archive {
    std::string path, id, version, sha256, manifestSha256, signature, manifest;
    uint64_t size;
};

struct Reply {
    bool ok;
    std::string text;           // the result object, or the error object, as JSON
    std::string code, message;  // of an error
    Reply() : ok(false) {}
};

std::string number(uint64_t value) {
    char text[32];
    snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return text;
}

class Controller : public PeerHandler {
public:
    Controller()
        : peer_(0), nextStream_(1), streaming_(false), stream_(0), left_(0), file_(0), memory_(0), memoryAt_(0),
          archiveStream_(false), archiveBytes_(0), cancelAfter_(-1), dropAfter_(-1), cancelSent_(false), done_(false),
          quiet_(false), restarting_(false), pullId_(0), pullStream_(0), pullSize_(0), pullWire_(0), pullBytes_(0),
          pullOut_(0), pullInflater_(0), pullEnded_(false) {}
    ~Controller() {
        if (file_ != 0) fclose(file_);
        delete pullInflater_;
        delete peer_;
    }

    bool connect(const std::string& path) {
        std::string why;
        Link* link = connectLocal(path, &why);
        if (link == 0) {
            fprintf(stderr, "nbtctl: %s\n", why.c_str());
            return false;
        }
        peer_ = new Peer(link, true, this);
        json::Writer w;
        w.beginObject().key("protocolMin").integer(1).key("protocolMax").integer(1).key("role").string("cli");
        w.key("maxFrame").integer(kMaxFramePayload).key("capabilities").beginArray().endArray().endObject();
        Reply reply;
        if (!call("hello", w.text(), &reply)) return false;
        if (!reply.ok) {
            fprintf(stderr, "nbtctl: the agent refused hello: %s: %s\n", reply.code.c_str(), reply.message.c_str());
            return false;
        }
        return true;
    }

    // Sends a request and serves the link until its answer is there. False if the link broke first.
    bool call(const std::string& method, const std::string& params, Reply* out) {
        int64_t id = peer_->request(method, params);
        if (id == 0) {
            fprintf(stderr, "nbtctl: %s does not fit in a frame (too many packages in the folder?)\n", method.c_str());
            return false;
        }
        while (replies_.count(id) == 0) {
            if (!pump(1000)) return false;
        }
        *out = replies_[id];
        replies_.erase(id);
        return true;
    }

    // Serves the link once: reads what came, writes what is queued, and feeds an open stream.
    bool pump(int timeoutMs) {
        if (!peer_->open()) return lost();
        feed();
        fd_set readable, writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        int fd = peer_->fd();
        FD_SET(fd, &readable);
        if (peer_->wantsWrite()) FD_SET(fd, &writable);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (streaming_ && !peer_->wantsWrite()) tv.tv_sec = tv.tv_usec = 0;
        if (select(fd + 1, &readable, &writable, 0, &tv) > 0) {
            if (FD_ISSET(fd, &writable)) peer_->flush();
            if (peer_->open() && FD_ISSET(fd, &readable)) peer_->readable();
        }
        return peer_->open() || lost();
    }

    // files.fetch with these params, its stream written to `out`. True when the stream is whole; false with *why
    // set otherwise, and with *linkBroke if that is why.
    bool pull(const std::string& params, FILE* out, std::string* why, bool* linkBroke) {
        *linkBroke = false;
        json::Document doc;
        doc.parse(params, 0);
        json::Value compress = doc.root().find("compress");
        pullOut_ = out;
        pullInflater_ = compress.isString() && compress.string() == "deflate" ? new Inflater() : 0;
        pullId_ = peer_->request("files.fetch", params);
        if (pullId_ == 0) {
            *why = "files.fetch does not fit in a frame";
            return false;
        }
        // The DATA frames may come in the same read as the reply: onResponse takes the stream's number at once.
        while (!pullEnded_) {
            if (!pump(1000)) {
                *linkBroke = true;
                *why = "the link ended";
                return false;
            }
        }
        *why = pullError_;
        return pullError_.empty();
    }
    uint64_t pulledBytes() const { return pullBytes_; }
    uint64_t pulledWire() const { return pullWire_; }

    // Until the job that is running reports job.done. False if the link broke.
    bool waitForJob() {
        while (!done_) {
            if (!pump(1000)) return false;
        }
        return true;
    }

    // Until the pairing window this controller opened has closed. False if the link broke.
    bool waitForPairing() {
        while (pairingClosed_.empty()) {
            if (!pump(1000)) return false;
        }
        return true;
    }
    const std::string& pairingClosed() const { return pairingClosed_; }

    // Reads the folder's packages. One that the agent's keys do not verify is left out, with a remark: the agent
    // would refuse it anyway.
    bool scan(const std::string& dir, const std::vector<TrustedKey>& keys) {
        std::vector<std::string> names;
        std::string why;
        if (!fs::listDir(dir, &names, &why)) {
            fprintf(stderr, "nbtctl: %s\n", why.c_str());
            return false;
        }
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string& name = names[i];
            if (name.size() < 8 || name.compare(name.size() - 7, 7, ".nbtpkg") != 0) continue;
            FileSource source;
            Package package;
            Error error;
            if (!source.open(dir + "/" + name, &why)) {
                fprintf(stderr, "nbtctl: skipped %s: %s\n", name.c_str(), why.c_str());
                continue;
            }
            if (!package.open(&source, keys, &error)) {
                fprintf(stderr, "nbtctl: skipped %s: %s: %s\n", name.c_str(), error.code.c_str(), error.message.c_str());
                continue;
            }
            Archive a;
            a.path = dir + "/" + name;
            a.id = package.manifest().id;
            a.version = package.manifest().version;
            a.sha256 = package.sha256();
            a.size = package.size();
            a.manifestSha256 = package.manifestSha256();
            a.manifest = package.manifestBytes();
            a.signature = toHex((const uint8_t*)package.signatureBytes().data(), package.signatureBytes().size());
            archives_.push_back(a);
        }
        return true;
    }

    const std::vector<Archive>& archives() const { return archives_; }
    void setTestStops(long long cancelAfter, long long dropAfter) {
        cancelAfter_ = cancelAfter;
        dropAfter_ = dropAfter;
    }
    void setQuiet(bool quiet) { quiet_ = quiet; }
    const std::string& doneText() const { return doneText_; }

    // ---- PeerHandler
    virtual void onRequest(Peer* peer, int64_t id, const std::string& method, const json::Value& params) {
        if (method == "ping") {
            peer->respond(id, "{}");
            return;
        }
        if (method != "blob.fetch") {
            peer->respondError(id, "unsupported", "a controller answers blob.fetch and ping only");
            return;
        }
        json::Value sha = params.find("sha256"), offset = params.find("offset"), length = params.find("length");
        if (!sha.isString() || !offset.isInteger() || offset.integer() < 0 ||
            (length.present() && (!length.isInteger() || length.integer() < 1))) {
            peer->respondError(id, "bad-request", "blob.fetch takes sha256, offset and an optional length");
            return;
        }
        if (streaming_) {
            peer->respondError(id, "busy", "a stream is open");
            return;
        }
        const Archive* archive = 0;
        bool wantManifest = false;
        for (size_t i = 0; i < archives_.size() && archive == 0; ++i) {
            if (archives_[i].sha256 == sha.string()) {
                archive = &archives_[i];
            } else if (archives_[i].manifestSha256 == sha.string()) {
                archive = &archives_[i];
                wantManifest = true;
            }
        }
        if (archive == 0) {
            peer->respondError(id, "not-found", "no such blob here");
            return;
        }
        uint64_t size = wantManifest ? archive->manifest.size() : archive->size;
        uint64_t from = (uint64_t)offset.integer();
        uint64_t count = length.present() ? (uint64_t)length.integer() : (from < size ? size - from : 0);
        if (from >= size || count > size - from) {
            peer->respondError(id, "out-of-range", "the range is not inside the blob");
            return;
        }
        if (wantManifest) {
            memory_ = &archive->manifest;
            memoryAt_ = (size_t)from;
        } else {
            file_ = fopen(archive->path.c_str(), "rb");
            if (file_ == 0 || fseek(file_, (long)from, SEEK_SET) != 0) {
                if (file_ != 0) fclose(file_);
                file_ = 0;
                peer->respondError(id, "not-found", "the file has gone");
                return;
            }
        }
        streaming_ = true;
        archiveStream_ = !wantManifest;
        stream_ = nextStream_;
        nextStream_ = nextStream_ == 65535 ? 1 : (uint16_t)(nextStream_ + 1);
        left_ = count;
        json::Writer w;
        w.beginObject().key("stream").integer(stream_).key("size").integer((int64_t)count).endObject();
        peer->respond(id, w.text());
    }

    virtual void onResponse(Peer*, int64_t id, bool ok, const json::Value& result, const std::string& code,
                            const std::string& message) {
        if (pullId_ != 0 && id == pullId_) {
            pullId_ = 0;
            json::Value stream = result.find("stream"), size = result.find("size");
            if (!ok) {
                endPull(code + ": " + message);
            } else if (!size.isInteger() || size.integer() < 0) {
                endPull("the reply has no size");
            } else if (size.integer() == 0) {
                endPull("");   // nothing follows
            } else if (!stream.isInteger() || stream.integer() < 1 || stream.integer() > 65535) {
                endPull("the reply has no stream");
            } else {
                pullStream_ = (uint16_t)stream.integer();
                pullSize_ = (uint64_t)size.integer();
            }
            return;
        }
        Reply reply;
        reply.ok = ok;
        if (ok) {
            reply.text = jsonText(result);
        } else {
            reply.code = code;
            reply.message = message;
            json::Writer w;
            w.beginObject().key("code").string(code).key("message").string(message).endObject();
            reply.text = w.text();
        }
        replies_[id] = reply;
    }

    virtual void onEvent(Peer*, const std::string& event, int64_t, const json::Value& params) {
        if (event == "job.progress") {
            json::Value state = params.find("state"), step = params.find("step"), steps = params.find("steps");
            json::Value id = params.find("id"), phase = params.find("phase");
            if (!state.isString() || !step.isInteger() || !steps.isInteger() || !id.isString()) return;
            // The phase says more than the state while there is one; "paused" has to show either way.
            std::string what = phase.isString() && state.string() != "paused" ? phase.string() : state.string();
            std::string line = "[" + number((uint64_t)step.integer()) + "/" + number((uint64_t)steps.integer()) + "] " +
                               id.string() + ": " + what;
            if (line != lastProgress_ && !quiet_) printf("%s\n", line.c_str());
            lastProgress_ = line;
            if (state.string() == "restarting" && !restarting_) {
                restarting_ = true;
                printf("The agent is restarting to finish its own update. The link ends with it; `nbtctl status`, once the\n"
                       "new version is up, is what confirms that version and shows how the job went.\n");
            }
        } else if (event == "job.log") {
            json::Value source = params.find("source"), text = params.find("text");
            if (!source.isString() || !text.isString() || quiet_) return;
            printLog(source.string(), text.string());
        } else if (event == "job.done") {
            done_ = true;
            doneText_ = jsonText(params);
        } else if (event == "files.error") {
            json::Value stream = params.find("stream"), message = params.find("message");
            if (pullStream_ != 0 && stream.isInteger() && stream.integer() == pullStream_) {
                endPull("the agent ended the stream: " + (message.isString() ? message.string() : std::string("io-error")));
            }
        } else if (event == "pairing.closed") {
            json::Value reason = params.find("reason");
            pairingClosed_ = reason.isString() && !reason.string().empty() ? reason.string() : std::string("closed");
        }
        fflush(stdout);
    }

    // DATA from the agent: the stream of this controller's files.fetch, and nothing else.
    virtual void onData(Peer* peer, uint16_t stream, const std::string& bytes, bool last) {
        if (pullStream_ == 0 || stream != pullStream_) {
            peer->close("the agent sent a DATA frame for a stream that is not open");
            return;
        }
        pullWire_ += bytes.size();
        if (pullInflater_ == 0) {
            pullBytes_ += bytes.size();
            if (pullBytes_ > pullSize_) return endPull("the stream is longer than its reply said");
            if (!bytes.empty() && fwrite(bytes.data(), 1, bytes.size(), pullOut_) != bytes.size()) return endPull("the file cannot be written");
        } else {
            const uint8_t* in = (const uint8_t*)bytes.data();
            size_t left = bytes.size();
            Inflater::Result result = Inflater::kNeedInput;
            do {
                uint8_t buffer[16384];
                size_t produced = 0;
                result = pullInflater_->inflate(&in, &left, buffer, sizeof buffer, &produced);
                if (result == Inflater::kError) return endPull("the stream does not inflate");
                pullBytes_ += produced;
                if (pullBytes_ > pullSize_) return endPull("the stream inflates to more than its reply said");
                if (produced != 0 && fwrite(buffer, 1, produced, pullOut_) != produced) return endPull("the file cannot be written");
            } while (result == Inflater::kOutputFull || (result == Inflater::kNeedInput && left != 0));
            if (result == Inflater::kDone && (left != 0 || !last)) return endPull("the deflate stream ends before the last frame");
            if (last && result != Inflater::kDone) return endPull("the deflate stream does not end in the last frame");
        }
        if (last) endPull(pullBytes_ == pullSize_ ? "" : "the stream ended at " + number(pullBytes_) + " of " + number(pullSize_) + " bytes");
    }

    static void printLog(const std::string& source, const std::string& text) {
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            printf("    %s| %s\n", source.c_str(), text.substr(at, end - at).c_str());
            at = end + 1;
        }
    }

private:
    bool lost() {
        fprintf(stderr, "nbtctl: the link to the agent ended: %s\n", peer_->closeReason().c_str());
        return false;
    }

    // Queues DATA frames of the open stream, as long as the link keeps up.
    void feed() {
        while (streaming_ && peer_->open() && peer_->queued() < kQueueLow) {
            if (archiveStream_ && cancelAfter_ >= 0 && (long long)archiveBytes_ >= cancelAfter_ && !cancelSent_) {
                // The way a phone stops a job during the fetch: blob.error first, job.cancel second.
                cancelSent_ = true;
                json::Writer e;
                e.beginObject().key("stream").integer(stream_).key("code").string("cancelled");
                e.key("message").string("stopped with --cancel-after").endObject();
                peer_->event("blob.error", -1, e.text());
                endStream();
                json::Writer c;
                c.beginObject().key("planId").string(planId).endObject();
                peer_->request("job.cancel", c.text());
                return;
            }
            if (archiveStream_ && dropAfter_ >= 0 && (long long)archiveBytes_ >= dropAfter_) {
                while (peer_->wantsWrite()) peer_->flush();
                _exit(3);   // as a phone that walks out of range: no goodbye
            }
            char buffer[kMaxFramePayload];
            size_t want = peer_->maxFrame() < sizeof buffer ? peer_->maxFrame() : sizeof buffer;
            if (want > left_) want = (size_t)left_;
            // A test stop lands exactly on its byte count.
            long long stop = cancelAfter_ >= 0 && !cancelSent_ ? cancelAfter_ : dropAfter_;
            if (archiveStream_ && stop >= 0 && (long long)(archiveBytes_ + want) > stop) want = (size_t)(stop - (long long)archiveBytes_);
            size_t got = 0;
            if (memory_ != 0) {
                got = want;
                memcpy(buffer, memory_->data() + memoryAt_, got);
                memoryAt_ += got;
            } else {
                got = fread(buffer, 1, want, file_);
            }
            if (got != want) {
                json::Writer e;
                e.beginObject().key("stream").integer(stream_).key("code").string("io-error");
                e.key("message").string("the file could not be read").endObject();
                peer_->event("blob.error", -1, e.text());
                endStream();
                return;
            }
            left_ -= got;
            if (archiveStream_) archiveBytes_ += got;
            peer_->data(stream_, buffer, got, left_ == 0);
            if (left_ == 0) endStream();
        }
    }

    void endPull(const std::string& error) {
        pullError_ = error;
        pullEnded_ = true;
        pullStream_ = 0;
    }

    void endStream() {
        streaming_ = false;
        if (file_ != 0) fclose(file_);
        file_ = 0;
        memory_ = 0;
    }

public:
    std::string planId;   // of the job being served, for --cancel-after

private:
    Peer* peer_;
    std::map<int64_t, Reply> replies_;
    std::vector<Archive> archives_;

    uint16_t nextStream_;
    bool streaming_;
    uint16_t stream_;
    uint64_t left_;
    FILE* file_;
    const std::string* memory_;
    size_t memoryAt_;
    bool archiveStream_;
    uint64_t archiveBytes_;
    long long cancelAfter_, dropAfter_;
    bool cancelSent_;

    bool done_;
    std::string doneText_;
    std::string lastProgress_;
    bool quiet_;
    std::string pairingClosed_;   // the reason of pairing.closed, once it has come
    bool restarting_;   // the job has said that the agent restarts for its own update

    // The one files.fetch of `pull`.
    int64_t pullId_;           // its request, until the reply has come
    uint16_t pullStream_;      // then its stream, until the stream has ended
    uint64_t pullSize_;        // the file's bytes, as the reply said
    uint64_t pullWire_;        // bytes in DATA frames
    uint64_t pullBytes_;       // bytes written
    FILE* pullOut_;
    Inflater* pullInflater_;   // 0 for compress: none
    bool pullEnded_;
    std::string pullError_;
};

int usage() {
    fprintf(stderr,
            "usage: nbtctl [--socket PATH] COMMAND ...\n"
            "  info | list | sources\n"
            "  plan    --from DIR [ID[@VERSION]...] [--remove ID...] [--delete-data]\n"
            "  install --from DIR ID[@VERSION]... [--remove ID...] [--delete-data]\n"
            "  remove  [--delete-data] ID...\n"
            "  resume  --from DIR\n"
            "  status  [--events]\n"
            "  cancel\n"
            "  restart hmi|reboot\n"
            "  pair                     open the pairing window and print its code\n"
            "  phones                   the paired phones\n"
            "  forget  PHONEID | --all  end a phone's pairing\n"
            "  autostart [on|off]       whether the agent is started with the head unit; switch it on or off\n"
            "  logs    ID               the log files of a package (org.nbtapp.agent: the agent's own)\n"
            "  pull    ID PATH --out FILE [--offset N] [--length N] [--none]\n"
            "  call METHOD [JSON]\n"
            "The socket is $NBT_AGENT_SOCKET if --socket is not given"
#ifdef __QNXNTO__
            ", and %s if that is not set either.\n",
            kHeadUnitSocket
#else
            ".\n"
#endif
            );
    return 2;
}

int refused(const char* what, const Reply& reply) {
    fprintf(stderr, "nbtctl: %s: %s: %s\n", what, reply.code.c_str(), reply.message.c_str());
    return 1;
}

std::string text(json::Value object, const char* name) {
    json::Value v = object.find(name);
    return v.isString() ? v.string() : std::string();
}

long long integer(json::Value object, const char* name) {
    json::Value v = object.find(name);
    return v.isInteger() ? (long long)v.integer() : 0;
}

// The agent's trusted keys, to read the folder's packages with.
bool fetchKeys(Controller* c, std::vector<TrustedKey>* keys, int* status) {
    Reply reply;
    if (!c->call("sources.list", "{}", &reply)) {
        *status = 3;
        return false;
    }
    if (!reply.ok) {
        *status = refused("sources.list", reply);
        return false;
    }
    json::Document doc;
    doc.parse(reply.text, 0);
    json::Value sources = doc.root().find("sources");
    for (size_t i = 0; i < sources.size(); ++i) {
        TrustedKey key;
        if (!fromHex(text(sources.at(i), "key"), key.key, sizeof key.key)) continue;
        key.name = text(sources.at(i), "name");
        keys->push_back(key);
    }
    return true;
}

void printSteps(json::Value steps, bool withResult) {
    for (size_t i = 0; i < steps.size(); ++i) {
        json::Value s = steps.at(i);
        std::string line = "  " + text(s, "action") + " " + text(s, "id") + " " + text(s, "version");
        if (!text(s, "oldVersion").empty()) line += " (from " + text(s, "oldVersion") + ")";
        if (withResult) {
            line += ": " + text(s, "result");
            json::Value error = s.find("error");
            if (error.isObject()) line += " (" + text(error, "code") + ": " + text(error, "message") + ")";
        }
        printf("%s\n", line.c_str());
    }
}

// Prints a plan. True if it can run.
bool printPlan(json::Value plan) {
    printf("steps:\n");
    printSteps(plan.find("steps"), false);
    json::Value missing = plan.find("missing"), problems = plan.find("problems"), flags = plan.find("flags");
    for (size_t i = 0; i < missing.size(); ++i) {
        json::Value m = missing.at(i);
        printf("missing: %s %s (required by %s)\n", text(m, "id").c_str(), text(m, "version").c_str(), text(m, "requiredBy").c_str());
    }
    for (size_t i = 0; i < problems.size(); ++i) {
        json::Value p = problems.at(i);
        printf("problem: %s %s: %s\n", text(p, "code").c_str(), text(p, "id").c_str(), text(p, "message").c_str());
    }
    std::string all;
    for (size_t i = 0; i < flags.size(); ++i) all += (i == 0 ? "" : " ") + (flags.at(i).isString() ? flags.at(i).string() : std::string());
    printf("flags: %s\n", all.empty() ? "none" : all.c_str());
    printf("fetch %lld bytes, install %lld bytes, restart: %s\n", integer(plan, "fetchBytes"), integer(plan, "installBytes"),
           text(plan, "restart").c_str());
    std::string planId = text(plan, "planId");
    if (!planId.empty()) printf("plan %s\n", planId.c_str());
    return !planId.empty();
}

// Prints how a job ended. True if it succeeded.
bool printDone(const std::string& doneText) {
    json::Document doc;
    if (!doc.parse(doneText, 0)) return false;
    std::string state = text(doc.root(), "state");
    printf("job %s\n", state.c_str());
    printSteps(doc.root().find("steps"), true);
    std::string restart = text(doc.root(), "restart");
    if (restart != "none" && !restart.empty()) printf("restart needed: %s\n", restart.c_str());
    return state == "succeeded";
}

void printJob(json::Value job) {
    std::string line = "job " + text(job, "planId") + ": " + text(job, "state");
    if (job.find("result").isObject()) {
        printf("%s\n", line.c_str());
        printSteps(job.find("result").find("steps"), true);
        return;
    }
    line += ", step " + number((uint64_t)integer(job, "step")) + " of " + number((uint64_t)integer(job, "steps")) + " (" + text(job, "id") + ")";
    if (!text(job, "phase").empty()) line += ", " + text(job, "phase");
    if (job.find("bytes").isInteger()) line += ", " + number((uint64_t)integer(job, "bytes")) + " of " + number((uint64_t)integer(job, "total")) + " bytes";
    printf("%s\n", line.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    std::string socketPath = getenv("NBT_AGENT_SOCKET") != 0 ? getenv("NBT_AGENT_SOCKET") : "";
    std::vector<std::string> args;
    std::string from;
    std::vector<std::string> removes;
    bool deleteData = false, events = false, all = false, uncompressed = false;
    long long cancelAfter = -1, dropAfter = -1, offset = -1, length = -1;
    std::string out;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : 0;
        if (arg == "--socket" && value != 0) {
            socketPath = value;
            ++i;
        } else if (arg == "--from" && value != 0) {
            from = value;
            ++i;
        } else if (arg == "--remove" && value != 0) {
            removes.push_back(value);
            ++i;
        } else if (arg == "--cancel-after" && value != 0) {
            cancelAfter = atoll(value);
            ++i;
        } else if (arg == "--drop-after" && value != 0) {
            dropAfter = atoll(value);
            ++i;
        } else if (arg == "--out" && value != 0) {
            out = value;
            ++i;
        } else if (arg == "--offset" && value != 0) {
            offset = atoll(value);
            ++i;
        } else if (arg == "--length" && value != 0) {
            length = atoll(value);
            ++i;
        } else if (arg == "--none") {
            uncompressed = true;
        } else if (arg == "--delete-data") {
            deleteData = true;
        } else if (arg == "--events") {
            events = true;
        } else if (arg == "--all") {
            all = true;
        } else if (arg.compare(0, 2, "--") == 0) {
            return usage();
        } else {
            args.push_back(arg);
        }
    }
#ifdef __QNXNTO__
    if (socketPath.empty()) socketPath = kHeadUnitSocket;
#endif
    if (args.empty() || socketPath.empty()) return usage();
    std::string command = args[0];
    args.erase(args.begin());

    Controller c;
    if (!c.connect(socketPath)) return 3;
    c.setTestStops(cancelAfter, dropAfter);
    Reply reply;
    json::Document doc;

    if (command == "info") {
        if (!c.call("info.get", "{}", &reply)) return 3;
        if (!reply.ok) return refused("info.get", reply);
        doc.parse(reply.text, 0);
        json::Value r = doc.root(), free = r.find("free");
        printf("agentVersion %s\nheadUnitId %s\nmanifestVersion %lld\n", text(r, "agentVersion").c_str(),
               text(r, "headUnitId").c_str(), integer(r, "manifestVersion"));
        for (size_t i = 0; i < free.size(); ++i) {
            printf("free.%s %lld\n", free.key(i).c_str(), free.at(i).isInteger() ? (long long)free.at(i).integer() : 0);
        }
        printf("restartNeeded %s\n", text(r, "restartNeeded").c_str());
        return 0;
    }

    if (command == "list") {
        std::string after;
        for (;;) {
            json::Writer w;
            w.beginObject();
            if (!after.empty()) w.key("after").string(after);
            w.endObject();
            if (!c.call("packages.list", w.text(), &reply)) return 3;
            if (!reply.ok) return refused("packages.list", reply);
            doc.parse(reply.text, 0);
            json::Value packages = doc.root().find("packages");
            for (size_t i = 0; i < packages.size(); ++i) {
                json::Value p = packages.at(i), flags = p.find("flags");
                std::string all;
                for (size_t f = 0; f < flags.size(); ++f) all += (f == 0 ? "" : ",") + (flags.at(f).isString() ? flags.at(f).string() : std::string());
                printf("%s %s %s\n", text(p, "id").c_str(), text(p, "version").c_str(), all.empty() ? "-" : all.c_str());
                after = text(p, "id");
            }
            json::Value more = doc.root().find("more");
            if (packages.size() == 0 || !more.isBool() || !more.boolean()) return 0;
        }
    }

    if (command == "sources") {
        if (!c.call("sources.list", "{}", &reply)) return 3;
        if (!reply.ok) return refused("sources.list", reply);
        doc.parse(reply.text, 0);
        json::Value sources = doc.root().find("sources");
        for (size_t i = 0; i < sources.size(); ++i) {
            json::Value s = sources.at(i), embedded = s.find("embedded");
            printf("%s %s %s\n", text(s, "key").c_str(), embedded.isBool() && embedded.boolean() ? "embedded" : "added",
                   text(s, "name").c_str());
        }
        return 0;
    }

    if (command == "call") {
        if (args.empty()) return usage();
        if (!c.call(args[0], args.size() > 1 ? args[1] : "{}", &reply)) return 3;
        printf("%s\n", reply.text.c_str());
        return reply.ok ? 0 : 1;
    }

    if (command == "pair") {
        if (!args.empty()) return usage();
        if (!c.call("pairing.open", "{}", &reply)) return 3;
        if (!reply.ok) return refused("pairing.open", reply);
        doc.parse(reply.text, 0);
        // The code first and alone on its line, and out at once: a script reads it from there.
        printf("code %s\n", text(doc.root(), "code").c_str());
        printf("Type it into the phone app within %lld s. Waiting for the phone...\n", integer(doc.root(), "expiresIn"));
        fflush(stdout);
        if (!c.waitForPairing()) return 3;
        if (c.pairingClosed() == "issued") {
            printf("paired\n");
            return 0;
        }
        printf("not paired: %s\n", c.pairingClosed().c_str());
        return 1;
    }

    if (command == "phones") {
        if (!args.empty()) return usage();
        if (!c.call("phones.list", "{}", &reply)) return 3;
        if (!reply.ok) return refused("phones.list", reply);
        doc.parse(reply.text, 0);
        json::Value phones = doc.root().find("phones");
        for (size_t i = 0; i < phones.size(); ++i) {
            json::Value p = phones.at(i);
            time_t at = (time_t)integer(p, "pairedAt");
            struct tm when;
            char date[32] = "-";
            if (gmtime_r(&at, &when) != 0) strftime(date, sizeof date, "%Y-%m-%dT%H:%MZ", &when);
            // The name last: it is the one field that may hold a space.
            printf("%s %s %s\n", text(p, "phoneId").c_str(), date, text(p, "name").c_str());
        }
        return 0;
    }

    if (command == "forget") {
        if (all ? !args.empty() : args.size() != 1) return usage();
        json::Writer w;
        w.beginObject();
        if (all) w.key("all").boolean(true);
        else w.key("phoneId").string(args[0]);
        w.endObject();
        if (!c.call("phones.forget", w.text(), &reply)) return 3;
        if (!reply.ok) return refused("phones.forget", reply);
        doc.parse(reply.text, 0);
        long long count = integer(doc.root(), "forgotten");
        printf("%lld %s forgotten\n", count, count == 1 ? "phone" : "phones");
        return 0;
    }

    if (command == "autostart") {
        if (args.size() > 1 || (args.size() == 1 && args[0] != "on" && args[0] != "off")) return usage();
        const char* method = args.empty() ? "autostart.get" : "autostart.set";
        if (!c.call(method, args.empty() ? "{}" : args[0] == "on" ? "{\"on\":true}" : "{\"on\":false}", &reply)) return 3;
        if (!reply.ok) return refused(method, reply);
        doc.parse(reply.text, 0);
        json::Value r = doc.root(), on = r.find("on"), available = r.find("available"), needs = r.find("needs");
        printf("autostart %s\n", on.isBool() && on.boolean() ? "on" : "off");
        if (available.isBool() && available.boolean()) {
            printf("available yes\n");
        } else if (needs.isObject()) {
            printf("available no %s %s %s\n", text(r, "reason").c_str(), text(needs, "id").c_str(), text(needs, "version").c_str());
        } else {
            printf("available no %s\n", text(r, "reason").c_str());
        }
        printf("starts %lld\n", integer(r, "starts"));
        return 0;
    }

    if (command == "logs") {
        if (args.size() != 1) return usage();
        json::Writer w;
        w.beginObject().key("package").string(args[0]).key("area").string("logs").endObject();
        if (!c.call("files.list", w.text(), &reply)) return 3;
        if (!reply.ok) return refused("files.list", reply);
        doc.parse(reply.text, 0);
        json::Value files = doc.root().find("files"), more = doc.root().find("more");
        for (size_t i = 0; i < files.size(); ++i) {
            json::Value f = files.at(i);
            time_t at = (time_t)integer(f, "modified");
            struct tm when;
            char date[32] = "-";
            if (gmtime_r(&at, &when) != 0) strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", &when);
            // The path last: it may hold a space.
            printf("%lld %s %s\n", integer(f, "size"), date, text(f, "path").c_str());
        }
        if (more.isBool() && more.boolean()) fprintf(stderr, "nbtctl: older files are not listed: the reply was full\n");
        return 0;
    }

    if (command == "pull") {
        if (args.size() != 2 || out.empty()) return usage();
        json::Writer w;
        w.beginObject().key("package").string(args[0]).key("area").string("logs").key("path").string(args[1]);
        if (offset >= 0) w.key("offset").integer(offset);
        if (length >= 0) w.key("length").integer(length);
        w.key("compress").string(uncompressed ? "none" : "deflate").endObject();
        FILE* file = fopen(out.c_str(), "wb");
        if (file == 0) {
            fprintf(stderr, "nbtctl: %s cannot be written\n", out.c_str());
            return 1;
        }
        std::string why;
        bool linkBroke = false;
        bool whole = c.pull(w.text(), file, &why, &linkBroke);
        if (fclose(file) != 0 && whole) {
            whole = false;
            why = "the file cannot be written";
        }
        if (linkBroke) return 3;
        if (!whole) {
            fprintf(stderr, "nbtctl: files.fetch: %s\n", why.c_str());
            return 1;
        }
        printf("pulled %llu bytes, %llu on the link\n", (unsigned long long)c.pulledBytes(), (unsigned long long)c.pulledWire());
        return 0;
    }

    if (command == "restart") {
        if (args.size() != 1 || (args[0] != "hmi" && args[0] != "reboot")) return usage();
        if (!c.call("system.restart", "{\"kind\":\"" + args[0] + "\"}", &reply)) return 3;
        if (!reply.ok) return refused("system.restart", reply);
        printf("restart (%s) accepted\n", args[0].c_str());
        return 0;
    }

    if (command == "status" || command == "cancel" || command == "resume") {
        if (!c.call("job.status", "{}", &reply)) return 3;
        if (!reply.ok) return refused("job.status", reply);
        doc.parse(reply.text, 0);
        json::Value job = doc.root().find("job");
        if (!job.isObject()) {
            printf("no job on record\n");
            return command == "status" ? 0 : 1;
        }
        std::string planId = text(job, "planId"), state = text(job, "state");
        if (command == "status") {
            printJob(job);
            int64_t after = 0;
            while (events) {
                json::Writer w;
                w.beginObject().key("planId").string(planId).key("after").integer(after).endObject();
                Reply page;
                json::Document pageDoc;
                if (!c.call("job.status", w.text(), &page)) return 3;
                if (!page.ok) return refused("job.status", page);
                pageDoc.parse(page.text, 0);
                json::Value list = pageDoc.root().find("events"), more = pageDoc.root().find("more");
                for (size_t i = 0; i < list.size(); ++i) {
                    json::Value e = list.at(i), params = e.find("params");
                    if (e.find("seq").isInteger()) after = e.find("seq").integer();
                    if (text(e, "event") == "job.log") Controller::printLog(text(params, "source"), text(params, "text"));
                }
                if (list.size() == 0 || !more.isBool() || !more.boolean()) break;
            }
            return 0;
        }
        json::Writer w;
        w.beginObject().key("planId").string(planId).endObject();
        if (command == "cancel") {
            if (!c.call("job.cancel", w.text(), &reply)) return 3;
            if (!reply.ok) return refused("job.cancel", reply);
            printf("job %s cancelled\n", planId.c_str());
            return 0;
        }
        // resume
        if (state == "succeeded" || state == "failed" || state == "cancelled") {
            printJob(job);
            return state == "succeeded" ? 0 : 1;
        }
        if (from.empty()) return usage();
        std::vector<TrustedKey> keys;
        int status = 0;
        if (!fetchKeys(&c, &keys, &status) || !c.scan(from, keys)) return status != 0 ? status : 1;
        c.planId = planId;
        if (!c.call("job.start", w.text(), &reply)) return 3;
        if (!reply.ok) return refused("job.start", reply);
        if (!c.waitForJob()) return 3;
        return printDone(c.doneText()) ? 0 : 1;
    }

    if (command == "plan" || command == "install" || command == "remove") {
        if (command == "remove") {
            removes.insert(removes.end(), args.begin(), args.end());
            args.clear();
        }
        if (command != "remove" && from.empty()) return usage();
        if (args.empty() && removes.empty()) return usage();
        if (!from.empty()) {
            std::vector<TrustedKey> keys;
            int status = 0;
            if (!fetchKeys(&c, &keys, &status) || !c.scan(from, keys)) return status != 0 ? status : 1;
        }
        const std::vector<Archive>& archives = c.archives();
        json::Writer w;
        w.beginObject().key("install").beginArray();
        for (size_t i = 0; i < args.size(); ++i) {
            std::string id = args[i], version;
            size_t at = id.find('@');
            if (at != std::string::npos) {
                version = id.substr(at + 1);
                id.erase(at);
            } else {
                // The highest version of it in the folder.
                for (size_t k = 0; k < archives.size(); ++k) {
                    int order = 0;
                    if (archives[k].id != id) continue;
                    if (version.empty() || (compareVersionStrings(archives[k].version, version, &order) && order > 0)) version = archives[k].version;
                }
                if (version.empty()) {
                    fprintf(stderr, "nbtctl: no package %s in %s\n", id.c_str(), from.c_str());
                    return 1;
                }
            }
            w.beginObject().key("id").string(id).key("version").string(version).endObject();
        }
        w.endArray().key("remove").beginArray();
        for (size_t i = 0; i < removes.size(); ++i) {
            w.beginObject().key("id").string(removes[i]).key("deleteData").boolean(deleteData).endObject();
        }
        w.endArray().key("offers").beginArray();
        for (size_t i = 0; i < archives.size(); ++i) {
            const Archive& a = archives[i];
            w.beginObject().key("id").string(a.id).key("version").string(a.version).key("sha256").string(a.sha256);
            w.key("size").integer((int64_t)a.size).key("manifestSha256").string(a.manifestSha256);
            w.key("signature").string(a.signature).endObject();
        }
        w.endArray().endObject();
        if (!c.call("job.plan", w.text(), &reply)) return 3;
        if (!reply.ok) return refused("job.plan", reply);
        doc.parse(reply.text, 0);
        bool runnable = printPlan(doc.root());
        if (command == "plan" || !runnable) return runnable ? 0 : 1;

        c.planId = text(doc.root(), "planId");
        json::Writer start;
        start.beginObject().key("planId").string(c.planId).endObject();
        if (!c.call("job.start", start.text(), &reply)) return 3;
        if (!reply.ok) return refused("job.start", reply);
        if (!c.waitForJob()) return 3;
        return printDone(c.doneText()) ? 0 : 1;
    }
    return usage();
}

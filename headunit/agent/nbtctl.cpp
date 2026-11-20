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
//   stop                                   stop the agent (agent.stop): it ends now, with its launcher, and the
//                                          iDrive plugin does not start it again until the head unit powers down
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
//   checks  [--list]                       the compatibility checks: what was put onto the head unit by hand; with
//                                          --list, every folder each one would move and every path it would remove
//   checks  fix ID                         runs that check, and its fix on what it found
//   serial  reset                          has the car end the phone's Bluetooth serial link (serial.reset)
//   serial  status                         how Bluetooth serial stands (serial.status)
//   serial  dial                           has the car dial the phones connected to it now, for trials (serial.dial)
//   channel PACKAGE NAME [--send TEXT] [--hex | --raw [--skip-first]]
//                                          opens a channel of an installed package and prints each message that
//                                          comes, a line each; --hex prints its bytes in hex; --send writes one
//                                          message first; --raw passes standard input and output through instead;
//                                          --skip-first puts the first message that comes on standard error
//   channel status|on|off PACKAGE NAME     whether a switched channel is on, and how many connections it has; switch
//                                          it on or off
//   settings ID                            the settings of an installed package, and how each stands in its file
//   set     ID KEY VALUE                   one setting changed: VALUE as the option's type takes it
//   unset   ID KEY                         one setting's line taken out of the file, so that the package's own
//                                          default holds
//   call METHOD [JSON]                     any request, the reply printed as it came
//
// `settings` prints a line for each option, in the manifest's order: its key, its state (set, unset or by-hand),
// when a change to it applies (now, app-start, hmi-restart, reboot), and then, for a value that is set, the value
// as JSON (true, 25, "text"), and for one set by hand, what the file has as a JSON string. `set` asks settings.get
// for the option's type first: a bool takes 1, on, yes, true or 0, off, no, false, an int its digits, a text or a
// select the word as it is; a VALUE its type does not read is a usage error. Both `set` and `unset` print "set KEY"
// or "unset KEY" when it is done; where the agent says the setting is not allowed, a line "KEY REASON" follows the
// error, for each key at fault (unknown-key, wrong-type, out-of-range, too-long, bad-characters, not-a-choice).
//
// `autostart` prints three lines: "autostart on" or "autostart off"; "available yes", or "available no" with the
// reason and, where it is the iDrive plugin that is wanting, its package and the versions that would do; and
// "starts N", the starts with the head unit on record that no agent outlasted by five minutes.
//
// `pull` prints one line when the file is whole: "pulled N bytes, M on the link". A stream that ends short, does
// not inflate, or that the agent ends with files.error leaves FILE as far as it came, says why, and exits with 1.
//
// `checks` prints a line for each check: its id and "ok", "unknown" and why, or "problem" and the number of paths
// it would remove, and "no fix" where it has none. With --list each problem's line is followed, two spaces in, by
// "move FROM to TO" for each folder it would move, and then by each path it would remove, all of them, a page at a
// time. `checks fix ID` runs the check and, where it finds a problem, the fix with the token of what it found: a
// line "moved FROM to TO" for each folder that was moved, "removed PATH" for each path that went, "left PATH" for
// each that is still there and each folder that was not moved, "stopped NAME" for each program that was stopped,
// then the check's id and the state it finds now, and "restart needed:" with the restart, unless none is. It exits
// with 1 if the check still finds a problem, or finds nothing it can say.
//
// `serial reset` prints "sent" or "armed" as its first line, and then what that means: "sent", that the car was
// asked to end the phone's serial link and the agent's log says what came of it; "armed", that no serial link was
// up, that the next one within 60 seconds is ended at its open, and that Connect is to be tapped in the phone app
// now. `nbtctl call serial.reset '{"device":"...","instance":"...","service":"..."}'` gives other values for the
// request.
//
// `serial status` prints a line for each member of serial.status's answer, "name value", with a member of an
// object as "object.name", and an array's items with ", " between them: "registered true", "dial on",
// "counts.dials 3". `serial dial` prints "dialling N phones", and the agent's log says what came of it.
//
// `stop` prints "stopping" as its first line, and then that the agent ends now with its launcher, and that the iDrive
// plugin does not start it again until the head unit has powered down; Start the agent in the plugin's menu, or the
// launcher started by hand, starts it before that. It comes back with the agent's answer and does not wait for the
// process to end. The agent refuses it with `busy` while a job runs.
//
// `channel` keeps to the credits both ways (agent.hpp, "Channels") and ends when the channel closes, with 0 if its
// program closed it and 1 for any other reason, which it says; with 3 when the link to the agent ends; and at SIGINT,
// SIGTERM or SIGHUP with 0, when it closes the channel itself. With --raw, standard input is cut into messages of up
// to 4,096 bytes as it is read, and each message that comes is written to standard output as it is, with nothing
// between them: so an SSH client can take it as its proxy command. The end of its input ends nothing: nbtctl stops
// reading and goes on writing what comes until one of the ends above (the client ends its proxy command with SIGHUP).
// Standard output is written without a buffer, in every mode: when it cannot be written (its reader has gone),
// `channel` closes the channel, says why, and exits with 1. Standard output carries the channel and nothing else: a
// job that another controller runs meanwhile is not printed, and while it holds the channel (channel.paused, then
// channel.resumed) a line on standard error says so at its start and at its end. With --raw, --skip-first takes the
// first message that comes out of that stream: it is written to standard error as one line (its bytes, each line end
// in it a space, and a newline), and standard output carries what comes after it. With --send that makes nbtctl the
// proxy command of an SSH client against a program that answers a first message before the stream begins, as the
// SSH package's relay answers the keys message with its host keys: the client reads the stream and the caller
// reads the answer. --skip-first without --raw is a usage error: there the first message is a line of its own.
// `channel status`, `on` and `off` print "switched", "on" and "open", a line each.
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
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <deque>
#include <map>
#include <set>
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
#include "settings.hpp"
#include "utf8.hpp"

using namespace nbt;

namespace {

const size_t kQueueLow = 256u << 10;   // DATA frames are queued only while less than this waits to be written

// Set by SIGINT, SIGTERM and SIGHUP while `channel` runs: it closes the channel and ends.
volatile sig_atomic_t gStop = 0;
void onStop(int) { gStop = 1; }

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
          pullOut_(0), pullInflater_(0), pullEnded_(false), chanOpenId_(0), chanStream_(0), chanMaySend_(0) {}
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
        json::Document doc;
        if (doc.parse(reply.text, 0) && doc.root().find("agentVersion").isString()) {
            agentVersion_ = doc.root().find("agentVersion").string();
        }
        return true;
    }

    // Sends a request and serves the link until its answer is there. False if the link broke first. An answer that
    // came together with the link's end counts: agent.stop's does, when the agent has ended before this reads.
    bool call(const std::string& method, const std::string& params, Reply* out) {
        int64_t id = peer_->request(method, params);
        if (id == 0) {
            fprintf(stderr, "nbtctl: %s does not fit in a frame (too many packages in the folder?)\n", method.c_str());
            return false;
        }
        while (replies_.count(id) == 0) {
            if (!serve(1000) && replies_.count(id) == 0) return lost();
        }
        *out = replies_[id];
        replies_.erase(id);
        return true;
    }

    // Serves the link once: reads what came, writes what is queued, and feeds an open stream. False, and said, if
    // the link has ended.
    bool pump(int timeoutMs) { return serve(timeoutMs) || lost(); }

    // pump() that says nothing: false if the link has ended, whatever else came with its end.
    bool serve(int timeoutMs) {
        if (!peer_->open()) return false;
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
        return peer_->open();
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
    // would refuse it anyway. Each is read for the agent this is linked to, and one that needs a later agent is
    // offered all the same, with the id and version its manifest names, even where the rest of its manifest has a
    // format only that later agent reads: the agent's plan then says agent-too-old, as it does for any such package.
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
            bool tooOld = false;
            if (!package.open(&source, keys, std::string(), agentVersion_, &error)) {
                tooOld = error.code == "agent-too-old";
                if (!tooOld) {
                    fprintf(stderr, "nbtctl: skipped %s: %s: %s\n", name.c_str(), error.code.c_str(), error.message.c_str());
                    continue;
                }
            }
            Archive a;
            a.path = dir + "/" + name;
            if (tooOld) {
                json::Document doc;
                doc.parse(package.manifestBytes(), 0);
                a.id = doc.root().find("id").isString() ? doc.root().find("id").string() : std::string();
                a.version = doc.root().find("version").isString() ? doc.root().find("version").string() : std::string();
                if (!validPackageId(a.id) || !validVersion(a.version)) {
                    fprintf(stderr, "nbtctl: skipped %s: %s: %s\n", name.c_str(), error.code.c_str(), error.message.c_str());
                    continue;
                }
            } else {
                a.id = package.manifest().id;
                a.version = package.manifest().version;
            }
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
        // The channel's first messages may come in the same read as this answer: its number is taken at once.
        if (chanOpenId_ != 0 && id == chanOpenId_ && ok) {
            json::Value stream = result.find("stream"), credit = result.find("credit");
            if (stream.isInteger() && stream.integer() >= kChannelFirstStream && stream.integer() <= 65535) chanStream_ = (uint16_t)stream.integer();
            if (credit.isInteger() && credit.integer() > 0) chanMaySend_ = (unsigned)credit.integer();
        }
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
        // The error object as a whole: some codes carry more than a message (settings.set's `invalid` has keys).
        reply.text = jsonText(result);
        if (!ok) {
            reply.code = code;
            reply.message = message;
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
                if (!quiet_) printf("The agent is restarting to finish its own update. The link ends with it; `nbtctl status`, once the\n"
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
        } else if (event == "channel.credit" || event == "channel.close") {
            json::Value stream = params.find("stream"), messages = params.find("messages"), reason = params.find("reason");
            if (chanStream_ == 0 || !stream.isInteger() || stream.integer() != chanStream_) return;
            if (event == "channel.credit" && messages.isInteger() && messages.integer() > 0) chanMaySend_ += (unsigned)messages.integer();
            if (event == "channel.close") chanClosed_ = reason.isString() && !reason.string().empty() ? reason.string() : std::string("closed");
        } else if (event == "channel.paused" || event == "channel.resumed") {
            // On standard error: standard output is the channel's.
            if (chanStream_ != 0) {
                fprintf(stderr, "%s\n", event == "channel.paused" ? "nbtctl: the channel is held while a job runs"
                                                                   : "nbtctl: the job is over: the channel goes on");
            }
        } else if (event == "pairing.closed") {
            json::Value reason = params.find("reason");
            pairingClosed_ = reason.isString() && !reason.string().empty() ? reason.string() : std::string("closed");
        }
        fflush(stdout);
    }

    // DATA from the agent: the stream of this controller's files.fetch, or its channel's messages, and nothing else.
    virtual void onData(Peer* peer, uint16_t stream, const std::string& bytes, bool last) {
        if (chanStream_ != 0 && stream == chanStream_) {
            chanIn_.push_back(bytes);
            return;
        }
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

    // `channel`: opens it, and serves it until it ends. The exit status.
    int channel(const std::string& package, const std::string& name, bool raw, bool hex, bool sendFirst, const std::string& first,
                bool skipFirst) {
        // Standard output is the channel's alone: a job that another controller runs meanwhile is not printed.
        quiet_ = true;
        json::Writer w;
        w.beginObject().key("package").string(package).key("channel").string(name).key("credit").integer(kChannelCredit).endObject();
        chanOpenId_ = peer_->request("channel.open", w.text());
        if (chanOpenId_ == 0) return 3;
        while (replies_.count(chanOpenId_) == 0) {
            if (!pump(1000)) return 3;
        }
        Reply reply = replies_[chanOpenId_];
        replies_.erase(chanOpenId_);
        chanOpenId_ = 0;
        if (!reply.ok) {
            fprintf(stderr, "nbtctl: channel.open: %s: %s\n", reply.code.c_str(), reply.message.c_str());
            return 1;
        }
        if (chanStream_ == 0) {
            fprintf(stderr, "nbtctl: channel.open gave no stream\n");
            return 3;
        }
        unsigned taken = 0;    // messages written out here since the agent was last granted more
        // The end of the input ends nothing: what the program says after it still comes (an SSH client's proxy
        // command is ended by the client, with SIGHUP).
        bool inputEnded = !raw;
        if (sendFirst && chanMaySend_ > 0) {
            peer_->data(chanStream_, first.data(), first.size(), true);
            --chanMaySend_;
        }
        for (;;) {
            // What came is written out first, whatever then ends the run. Straight to the descriptor: nothing waits
            // in a buffer of stdio, and a write that fails is known at once.
            std::string outputError;
            while (!chanIn_.empty() && outputError.empty() && !gStop) {
                const std::string& m = chanIn_.front();
                if (skipFirst) {
                    // The first message, to standard error as one line; the stream on standard output begins after it.
                    std::string line = m;
                    for (size_t i = 0; i < line.size(); ++i) {
                        if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
                    }
                    line += "\n";
                    fwrite(line.data(), 1, line.size(), stderr);
                    fflush(stderr);
                    skipFirst = false;
                    chanIn_.pop_front();
                    ++taken;
                    continue;
                }
                std::string line;
                if (hex) line = toHex((const uint8_t*)m.data(), m.size()) + "\n";
                else if (!raw) line = m + "\n";
                const std::string& bytes = hex || !raw ? line : m;
                if (!writeOut(bytes.data(), bytes.size(), &outputError)) break;
                chanIn_.pop_front();
                ++taken;
            }
            if (!outputError.empty()) {
                fprintf(stderr, "nbtctl: standard output: %s: the channel is closed\n", outputError.c_str());
                closeChannel();
                return 1;
            }
            if (taken >= kChannelCredit / 2 && peer_->open()) {
                json::Writer c;
                c.beginObject().key("stream").integer(chanStream_).key("messages").integer(taken).endObject();
                peer_->event("channel.credit", -1, c.text());
                taken = 0;
            }
            if (!chanClosed_.empty()) {
                if (chanClosed_ != "closed") fprintf(stderr, "nbtctl: the agent closed the channel: %s\n", chanClosed_.c_str());
                return chanClosed_ == "closed" ? 0 : 1;
            }
            if (!peer_->open()) {
                lost();
                return 3;
            }
            // Ended here: at a signal.
            if (gStop) {
                closeChannel();
                return 0;
            }
            fd_set readable, writable;
            FD_ZERO(&readable);
            FD_ZERO(&writable);
            int fd = peer_->fd(), top = fd;
            FD_SET(fd, &readable);
            if (peer_->wantsWrite()) FD_SET(fd, &writable);
            bool reading = raw && !inputEnded && chanMaySend_ > 0 && peer_->queued() < kQueueLow;
            if (reading) FD_SET(STDIN_FILENO, &readable);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if (select(top + 1, &readable, &writable, 0, &tv) > 0) {
                if (reading && FD_ISSET(STDIN_FILENO, &readable)) {
                    char buffer[kChannelMaxMessage];
                    ssize_t n = read(STDIN_FILENO, buffer, sizeof buffer);
                    if (n > 0) {
                        peer_->data(chanStream_, buffer, (size_t)n, true);
                        --chanMaySend_;
                    } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                        // From here on only the channel's own end, the link's, or a signal ends the run.
                        inputEnded = true;
                    }
                }
                if (FD_ISSET(fd, &writable)) peer_->flush();
                if (peer_->open() && FD_ISSET(fd, &readable)) peer_->readable();
            }
            // A link that has ended is seen at the top, once what came before its end has been written out.
        }
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

    // `channel`'s own close: channel.close to the agent, and a moment for the link to take it.
    void closeChannel() {
        if (!peer_->open()) return;
        json::Writer e;
        e.beginObject().key("stream").integer(chanStream_).key("reason").string("closed").endObject();
        peer_->event("channel.close", -1, e.text());
        for (int i = 0; i < 100 && peer_->open() && peer_->wantsWrite(); ++i) pump(20);
    }

    // All of `bytes` to standard output. False when that fails, with *why; and false with *why empty when a signal
    // that ends `channel` comes while it waits.
    static bool writeOut(const char* bytes, size_t length, std::string* why) {
        while (length > 0) {
            ssize_t n = write(STDOUT_FILENO, bytes, length);
            if (n > 0) {
                bytes += n;
                length -= (size_t)n;
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                if (gStop) return false;
                if (errno == EAGAIN) {
                    // An output that someone else made non-blocking: until it takes more.
                    fd_set writable;
                    FD_ZERO(&writable);
                    FD_SET(STDOUT_FILENO, &writable);
                    struct timeval tv;
                    tv.tv_sec = 1;
                    tv.tv_usec = 0;
                    select(STDOUT_FILENO + 1, 0, &writable, 0, &tv);
                }
                continue;
            }
            *why = n < 0 ? strerror(errno) : "nothing could be written";
            return false;
        }
        return true;
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
    std::string agentVersion_;   // from the agent's hello
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
    bool quiet_;    // nothing of a job is printed: its progress, its log, the agent's restart for it
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

    // The one channel of `channel`.
    int64_t chanOpenId_;             // its channel.open, until the answer has come
    uint16_t chanStream_;            // then its number
    unsigned chanMaySend_;           // the messages this side may still send
    std::deque<std::string> chanIn_;   // messages that came and are not written out yet
    std::string chanClosed_;         // the reason of the agent's channel.close, once it has come
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
            "  stop                     stop the agent; the iDrive plugin does not start it before the next power up\n"
            "  pair                     open the pairing window and print its code\n"
            "  phones                   the paired phones\n"
            "  forget  PHONEID | --all  end a phone's pairing\n"
            "  autostart [on|off]       whether the agent is started with the head unit; switch it on or off\n"
            "  logs    ID               the log files of a package (org.nbtapp.agent: the agent's own)\n"
            "  pull    ID PATH --out FILE [--offset N] [--length N] [--none]\n"
            "  checks  [--list]         what was put onto the head unit by hand\n"
            "  checks  fix ID           that check's fix, on the list it finds\n"
            "  serial  reset            have the car end the phone's Bluetooth serial link\n"
            "  serial  status           how Bluetooth serial stands\n"
            "  serial  dial             have the car dial the phones connected to it now\n"
            "  channel PACKAGE NAME [--send TEXT] [--hex | --raw [--skip-first]]\n"
            "                           open a channel: print what comes, or pass standard input and output through;\n"
            "                           --skip-first: the first message that comes to standard error, as a line\n"
            "  channel status|on|off PACKAGE NAME   whether a switched channel is on; switch it\n"
            "  settings ID              the settings of an installed package\n"
            "  set     ID KEY VALUE     change one of them\n"
            "  unset   ID KEY           take one out of its file: the package's default holds\n"
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

// Each member of an object as a line "name value", a member of an object within it as "object.name value", and an
// array's items with ", " between them.
void printMembers(json::Value object, const std::string& prefix) {
    for (size_t i = 0; i < object.size(); ++i) {
        json::Value v = object.at(i);
        std::string name = prefix + object.key(i);
        if (v.isObject()) {
            printMembers(v, name + ".");
            continue;
        }
        std::string shown;
        if (v.isArray()) {
            for (size_t k = 0; k < v.size(); ++k) {
                json::Value item = v.at(k);
                shown += (k ? ", " : "") + (item.isString() ? item.string() : std::string("?"));
            }
        } else if (v.isString()) {
            shown = v.string();
        } else if (v.isInteger()) {
            char n[32];
            snprintf(n, sizeof n, "%lld", (long long)v.integer());
            shown = n;
        } else if (v.isBool()) {
            shown = v.boolean() ? "true" : "false";
        }
        printf("%s %s\n", name.c_str(), shown.c_str());
    }
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

// Every path a check found: the ones in `first`, the check as checks.run gave it, and the pages after them. False
// with *status if the agent refused a page or the link broke.
bool checkPaths(Controller* c, const std::string& id, json::Value first, std::vector<std::string>* out, int* status) {
    json::Value found = first.find("found"), more = first.find("more");
    for (size_t i = 0; i < found.size(); ++i) out->push_back(found.at(i).isString() ? found.at(i).string() : std::string());
    bool again = more.isBool() && more.boolean();
    while (again) {
        json::Writer w;
        w.beginObject().key("id").string(id);
        if (!out->empty()) w.key("after").string(out->back());
        w.endObject();
        Reply reply;
        if (!c->call("checks.run", w.text(), &reply)) {
            *status = 3;
            return false;
        }
        if (!reply.ok) {
            *status = refused("checks.run", reply);
            return false;
        }
        json::Document doc;
        doc.parse(reply.text, 0);
        json::Value list = doc.root().find("checks");
        if (list.size() != 1) break;
        json::Value page = list.at(0).find("found"), next = list.at(0).find("more");
        // A page with nothing in it ends the list, so that a list that changes under it is not asked for ever.
        if (page.size() == 0) break;
        for (size_t i = 0; i < page.size(); ++i) out->push_back(page.at(i).isString() ? page.at(i).string() : std::string());
        again = next.isBool() && next.boolean();
    }
    return true;
}

// Each move of a list as a line: `prefix` FROM to TO.
void printMoves(const char* prefix, json::Value moves) {
    for (size_t i = 0; i < moves.size(); ++i) printf("%s%s to %s\n", prefix, text(moves.at(i), "from").c_str(), text(moves.at(i), "to").c_str());
}

// One check's line, as `checks` prints it.
void printCheck(json::Value check) {
    std::string id = text(check, "id"), state = text(check, "state");
    if (state == "unknown") printf("%s unknown: %s\n", id.c_str(), text(check, "why").c_str());
    else if (state == "problem") printf("%s problem %lld%s\n", id.c_str(), integer(check, "count"), check.find("fix").isObject() ? "" : " no fix");
    else printf("%s %s\n", id.c_str(), state.c_str());
}

// Every option of a package's settings, a page at a time, as JSON texts. False with *status if the agent refused a
// page or the link broke.
bool settingsOptions(Controller* c, const std::string& package, std::vector<std::string>* out, int* status) {
    int64_t from = 0;
    for (;;) {
        json::Writer w;
        w.beginObject().key("package").string(package).key("from").integer(from).endObject();
        Reply reply;
        if (!c->call("settings.get", w.text(), &reply)) {
            *status = 3;
            return false;
        }
        if (!reply.ok) {
            *status = refused("settings.get", reply);
            return false;
        }
        json::Document doc;
        doc.parse(reply.text, 0);
        json::Value options = doc.root().find("options"), next = doc.root().find("next");
        for (size_t i = 0; i < options.size(); ++i) out->push_back(jsonText(options.at(i)));
        // A page that takes the list no further ends it, so that a list that changes under it is not asked for ever.
        if (!next.isInteger() || next.integer() <= from || options.size() == 0) return true;
        from = next.integer();
    }
}

// settings.set with these params; prints "DONE KEY" when it is done, and each key's reason when it is not allowed.
int settingsChange(Controller* c, const std::string& params, const char* done, const std::string& key) {
    Reply reply;
    if (!c->call("settings.set", params, &reply)) return 3;
    if (!reply.ok) {
        int status = refused("settings.set", reply);
        json::Document doc;
        doc.parse(reply.text, 0);
        json::Value keys = doc.root().find("keys");
        for (size_t i = 0; i < keys.size(); ++i) printf("%s %s\n", text(keys.at(i), "key").c_str(), text(keys.at(i), "reason").c_str());
        return status;
    }
    printf("%s %s\n", done, key.c_str());
    return 0;
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
    bool deleteData = false, events = false, all = false, uncompressed = false, listPaths = false, hex = false, raw = false;
    bool sendFirst = false, skipFirst = false;
    std::string first;
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
        } else if (arg == "--list") {
            listPaths = true;
        } else if (arg == "--hex") {
            hex = true;
        } else if (arg == "--raw") {
            raw = true;
        } else if (arg == "--skip-first") {
            skipFirst = true;
        } else if (arg == "--send" && value != 0) {
            sendFirst = true;
            first = value;
            ++i;
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

    if (command == "channel" && args.size() == 3 && (args[0] == "status" || args[0] == "on" || args[0] == "off")) {
        if (!validPackageId(args[1])) return usage();
        json::Writer w;
        w.beginObject().key("package").string(args[1]).key("channel").string(args[2]);
        if (args[0] != "status") w.key("on").boolean(args[0] == "on");
        w.endObject();
        const char* method = args[0] == "status" ? "channel.get" : "channel.set";
        if (!c.call(method, w.text(), &reply)) return 3;
        if (!reply.ok) return refused(method, reply);
        doc.parse(reply.text, 0);
        printMembers(doc.root(), "");
        return 0;
    }

    if (command == "channel") {
        if (args.size() != 2 || !validPackageId(args[0]) || (hex && raw) || (skipFirst && !raw) ||
            (sendFirst && (first.empty() || first.size() > kChannelMaxMessage))) {
            return usage();
        }
        struct sigaction action;
        memset(&action, 0, sizeof action);
        action.sa_handler = onStop;
        sigaction(SIGINT, &action, 0);
        sigaction(SIGTERM, &action, 0);
        sigaction(SIGHUP, &action, 0);
        return c.channel(args[0], args[1], raw, hex, sendFirst, first, skipFirst);
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

    if (command == "checks") {
        if (args.empty()) {
            if (!c.call("checks.run", "{}", &reply)) return 3;
            if (!reply.ok) return refused("checks.run", reply);
            doc.parse(reply.text, 0);
            json::Value list = doc.root().find("checks");
            for (size_t i = 0; i < list.size(); ++i) {
                printCheck(list.at(i));
                if (!listPaths || text(list.at(i), "state") != "problem") continue;
                printMoves("  move ", list.at(i).find("moves"));
                std::vector<std::string> paths;
                int status = 0;
                if (!checkPaths(&c, text(list.at(i), "id"), list.at(i), &paths, &status)) return status;
                for (size_t k = 0; k < paths.size(); ++k) printf("  %s\n", paths[k].c_str());
            }
            return 0;
        }
        if (args.size() != 2 || args[0] != "fix" || listPaths) return usage();
        const std::string id = args[1];
        json::Writer w;
        w.beginObject().key("id").string(id).endObject();
        if (!c.call("checks.run", w.text(), &reply)) return 3;
        if (!reply.ok) return refused("checks.run", reply);
        doc.parse(reply.text, 0);
        json::Value list = doc.root().find("checks");
        if (list.size() != 1) {
            fprintf(stderr, "nbtctl: checks.run did not answer for %s alone\n", id.c_str());
            return 1;
        }
        json::Value check = list.at(0), fix = check.find("fix");
        std::string state = text(check, "state");
        if (state != "problem" || !fix.isObject()) {
            printCheck(check);
            if (state == "problem") printf("%s\n", text(check, "detail").c_str());
            return state == "ok" ? 0 : 1;
        }
        // The whole list, to say afterwards which of its paths went.
        std::vector<std::string> paths;
        int status = 0;
        if (!checkPaths(&c, id, check, &paths, &status)) return status;
        json::Writer f;
        f.beginObject().key("id").string(id).key("token").string(text(fix, "token")).endObject();
        Reply fixed;
        if (!c.call("checks.fix", f.text(), &fixed)) return 3;
        if (!fixed.ok) return refused("checks.fix", fixed);
        json::Document result;
        result.parse(fixed.text, 0);
        json::Value r = result.root(), left = r.find("left"), stopped = r.find("stopped");
        printMoves("moved ", r.find("moved"));
        // The paths of the list that are left; a folder that was not moved is in `left` too, and is not one of them.
        std::set<std::string> stayed;
        for (size_t i = 0; i < left.size(); ++i) stayed.insert(left.at(i).isString() ? left.at(i).string() : std::string());
        size_t listedStayed = 0;
        for (size_t i = 0; i < paths.size(); ++i) listedStayed += stayed.count(paths[i]);
        long long leftMore = integer(r, "leftMore");
        if (leftMore == 0 && (long long)(paths.size() - listedStayed) == integer(r, "removed")) {
            for (size_t i = 0; i < paths.size(); ++i) {
                if (stayed.count(paths[i]) == 0) printf("removed %s\n", paths[i].c_str());
            }
        } else {
            printf("removed %lld paths\n", integer(r, "removed"));
        }
        for (size_t i = 0; i < left.size(); ++i) printf("left %s\n", left.at(i).isString() ? left.at(i).string().c_str() : "");
        if (leftMore > 0) printf("left %lld more\n", leftMore);
        for (size_t i = 0; i < stopped.size(); ++i) printf("stopped %s\n", stopped.at(i).isString() ? stopped.at(i).string().c_str() : "");
        printf("%s %s\n", id.c_str(), text(r, "state").c_str());
        std::string restart = text(r, "restart");
        if (!restart.empty() && restart != "none") printf("restart needed: %s\n", restart.c_str());
        return text(r, "state") == "ok" ? 0 : 1;
    }

    if (command == "serial" && args.size() == 1 && args[0] == "status") {
        if (!c.call("serial.status", "{}", &reply)) return 3;
        if (!reply.ok) return refused("serial.status", reply);
        doc.parse(reply.text, 0);
        printMembers(doc.root(), "");
        return 0;
    }
    if (command == "serial" && args.size() == 1 && args[0] == "dial") {
        if (!c.call("serial.dial", "{}", &reply)) return 3;
        if (!reply.ok) return refused("serial.dial", reply);
        doc.parse(reply.text, 0);
        long long phones = integer(doc.root(), "phones");
        printf("dialling %lld phone%s\nThe agent's log says what came of it.\n", phones, phones == 1 ? "" : "s");
        return 0;
    }
    if (command == "serial") {
        if (args.size() != 1 || args[0] != "reset") return usage();
        if (!c.call("serial.reset", "{}", &reply)) return 3;
        if (!reply.ok) return refused("serial.reset", reply);
        doc.parse(reply.text, 0);
        std::string state = text(doc.root(), "state");
        if (state == "sent") {
            printf("sent\nThe car was asked to end the phone's serial link. The agent's log says what came of it.\n");
        } else if (state == "armed") {
            printf("armed\nNo serial link is up. The next one within %lld s is ended as soon as it opens: tap Connect "
                   "in the phone app now.\n", integer(doc.root(), "forMs") / 1000);
        } else {
            printf("%s\n", reply.text.c_str());
        }
        return 0;
    }

    if (command == "settings") {
        if (args.size() != 1) return usage();
        std::vector<std::string> options;
        int status = 0;
        if (!settingsOptions(&c, args[0], &options, &status)) return status;
        for (size_t i = 0; i < options.size(); ++i) {
            json::Document one;
            one.parse(options[i], 0);
            json::Value o = one.root();
            std::string line = text(o, "key") + " " + text(o, "state") + " " + text(o, "applies");
            // The value last: a text may hold a space.
            if (o.find("value").present()) line += " " + jsonText(o.find("value"));
            if (o.find("raw").present()) line += " " + jsonText(o.find("raw"));
            printf("%s\n", line.c_str());
        }
        return 0;
    }

    if (command == "set" || command == "unset") {
        if (args.size() != (command == "set" ? 3u : 2u)) return usage();
        const std::string& package = args[0];
        const std::string& key = args[1];
        json::Writer w;
        w.beginObject().key("package").string(package);
        if (command == "unset") {
            w.key("reset").beginArray().string(key).endArray().endObject();
            return settingsChange(&c, w.text(), "unset", key);
        }
        // The option's type, so that the value goes as the JSON it takes. A key with no option goes as a string,
        // and the agent says what it makes of it.
        std::vector<std::string> options;
        int status = 0;
        if (!settingsOptions(&c, package, &options, &status)) return status;
        std::string type;
        for (size_t i = 0; i < options.size() && type.empty(); ++i) {
            json::Document one;
            one.parse(options[i], 0);
            if (text(one.root(), "key") == key) type = text(one.root(), "type");
        }
        const std::string& value = args[2];
        w.key("set").beginObject().key(key);
        // Read by the agent's own rules for a file's value (settings.hpp), so that nbtctl takes what a file can hold.
        int64_t number = 0;
        int word = settingBoolWord(value);
        if (type == "bool") {
            if (word < 0) {
                fprintf(stderr, "nbtctl: %s is a bool: 1, on, yes, true or 0, off, no, false\n", key.c_str());
                return 2;
            }
            w.boolean(word == 1);
        } else if (type == "int") {
            if (!settingIntText(value, &number)) {
                fprintf(stderr, "nbtctl: %s is an int: digits, with a - before them for one below 0\n", key.c_str());
                return 2;
            }
            w.integer(number);
        } else {
            if (!validUtf8(value)) {
                fprintf(stderr, "nbtctl: the value is not UTF-8\n");
                return 2;
            }
            w.string(value);
        }
        w.endObject().endObject();
        return settingsChange(&c, w.text(), "set", key);
    }

    if (command == "restart") {
        if (args.size() != 1 || (args[0] != "hmi" && args[0] != "reboot")) return usage();
        if (!c.call("system.restart", "{\"kind\":\"" + args[0] + "\"}", &reply)) return 3;
        if (!reply.ok) return refused("system.restart", reply);
        printf("restart (%s) accepted\n", args[0].c_str());
        return 0;
    }

    if (command == "stop") {
        if (!args.empty()) return usage();
        if (!c.call("agent.stop", "{}", &reply)) return 3;
        if (!reply.ok) return refused("agent.stop", reply);
        printf("stopping\nThe agent ends now, and its launcher with it. The iDrive plugin does not start it again until the "
               "head unit has powered down; Start the agent in its menu starts it before that.\n");
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

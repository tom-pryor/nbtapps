// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/fsutil.cpp, agent/process.cpp, agent/journal.cpp and agent/json_writer.hpp: the file-system
// steps the engine is built from, the hook runner (output, exit codes, the timeout, a process left behind) and the
// journal's cut-off last line.
//
// usage: test_fsutil [work folder]     (default build/agent-host/work)

#include "../../agent/fsutil.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../agent/journal.hpp"
#include "../../agent/json.hpp"
#include "../../agent/json_writer.hpp"
#include "../../agent/process.hpp"
#include "test_util.hpp"

using namespace nbt;

static unsigned modeOf(const std::string& path) {
    struct stat st;
    CHECK(lstat(path.c_str(), &st) == 0);
    return (unsigned)(st.st_mode & 07777);
}

static std::string read(const std::string& path) {
    std::string data, why;
    CHECK_IN(why, fs::readFile(path, 1u << 20, &data, &why));
    return data;
}

class Collect : public OutputSink {
public:
    virtual void output(const char* data, size_t len) { text.append(data, len); }
    std::string text;
};

// A thread that does nothing but wait until the mutex is free.
static void* waitFor(void* mutex) {
    pthread_mutex_lock((pthread_mutex_t*)mutex);
    pthread_mutex_unlock((pthread_mutex_t*)mutex);
    return 0;
}

static ProcessResult sh(const std::string& script, const std::string& cwd, unsigned timeout, std::string* output) {
    std::vector<std::string> argv, env;
    argv.push_back("/bin/sh");
    argv.push_back("-c");
    argv.push_back(script);
    env.push_back("PATH=/usr/bin:/bin");
    env.push_back("NBT_TEST=value with spaces");
    Collect collect;
    ProcessResult result;
    std::string why;
    CHECK_IN(why, runProcess("/bin/sh", argv, env, cwd, timeout, &collect, &result, &why));
    if (output != 0) *output = collect.text;
    return result;
}

int main(int argc, char** argv) {
    std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host/work") + "/fsutil";
    char here[4096];
    CHECK(getcwd(here, sizeof here) != 0);
    if (work[0] != '/') work = std::string(here) + "/" + work;
    std::string why;
    CHECK(fs::removeTree(work, &why) && fs::makeDirs(work, &why));

    // 1. Paths.
    CHECK(fs::parentOf("/a/b") == "/a" && fs::parentOf("/a") == "/" && fs::parentOf("a") == "." && fs::parentOf("a/b/") == "a");
    CHECK(fs::parentOf("/") == "/" && fs::tempIn("/x") == "/x/.nbt-tmp");

    // 2. Folders.
    CHECK(fs::makeDirs(work + "/a/b/c", &why) && fs::kindOf(work + "/a/b/c") == fs::kDir && fs::makeDirs(work + "/a/b/c", &why));
    CHECK(fs::makeDir(work + "/a/one", &why));
    CHECK(!fs::makeDir(work + "/a/one", &why) && why.empty());             // there already: not an error
    CHECK(!fs::makeDir(work + "/missing/one", &why) && !why.empty());       // no parent: an error
    CHECK(fs::kindOf(work + "/nothing") == fs::kNone && !fs::isDir(work + "/nothing"));
    // Emptied: made if it is not there, and left as it is, the same folder untouched, if it holds nothing.
    {
        std::string dir = work + "/emptied";
        struct stat made, same, after;
        CHECK(fs::emptyDir(dir + "/in", &why) && fs::kindOf(dir + "/in") == fs::kDir && lstat(dir.c_str(), &made) == 0);
        CHECK(fs::emptyDir(dir, &why) && fs::kindOf(dir + "/in") == fs::kNone);
        struct timespec moment = {0, 20 * 1000 * 1000};
        nanosleep(&moment, 0);
        CHECK(lstat(dir.c_str(), &same) == 0 && fs::emptyDir(dir, &why) && lstat(dir.c_str(), &after) == 0);
        CHECK(same.st_ino == made.st_ino && after.st_ino == made.st_ino && after.st_mtime == same.st_mtime && after.st_ctime == same.st_ctime);
#ifndef __QNXNTO__
        CHECK(after.st_mtim.tv_nsec == same.st_mtim.tv_nsec && after.st_ctim.tv_nsec == same.st_ctim.tv_nsec);
#endif
        CHECK(fs::makeDirs(dir + "/a/b", &why) && fs::appendFile(dir + "/a/b/file", "x", &why) && fs::appendFile(dir + "/file", "x", &why));
        CHECK(fs::makeLink(work + "/a", dir + "/link", &why));
        CHECK(fs::emptyDir(dir, &why) && lstat(dir.c_str(), &after) == 0 && after.st_ino == made.st_ino);
        std::vector<std::string> left;
        CHECK(fs::listDir(dir, &left, &why) && left.empty() && fs::kindOf(work + "/a/b/c") == fs::kDir);   // the link went, not what it led to
        // Something that is not a folder gives way to one.
        CHECK(fs::removeTree(dir, &why) && fs::appendFile(dir, "a file", &why) && fs::emptyDir(dir, &why) && fs::kindOf(dir) == fs::kDir);
    }

    // 3. Files: written whole, with the mode asked for, and no temporary left.
    CHECK(fs::writeFile(work + "/a/file", "first", 0640, &why) && read(work + "/a/file") == "first" && modeOf(work + "/a/file") == 0640);
    CHECK(fs::writeFile(work + "/a/file", "second", 0755, &why) && read(work + "/a/file") == "second" && modeOf(work + "/a/file") == 0755);
    CHECK(fs::kindOf(fs::tempIn(work + "/a")) == fs::kNone);
    CHECK(!fs::writeFile(work + "/missing/file", "x", 0644, &why));
    std::string big;
    CHECK(!fs::readFile(work + "/a/file", 3, &big, &why) && !fs::readFile(work + "/a/none", 100, &big, &why));
    CHECK(fs::appendFile(work + "/a/log", "one\n", &why) && fs::appendFile(work + "/a/log", "two\n", &why) && read(work + "/a/log") == "one\ntwo\n");

    // A writer that is dropped leaves nothing, under either name; a leftover temporary is replaced.
    {
        fs::FileWriter writer;
        CHECK(writer.open(work + "/a/dropped", &why) && writer.write("half", 4));
        CHECK(fs::kindOf(fs::tempIn(work + "/a")) == fs::kFile);
    }
    CHECK(fs::kindOf(work + "/a/dropped") == fs::kNone && fs::kindOf(fs::tempIn(work + "/a")) == fs::kNone);
    CHECK(fs::appendFile(fs::tempIn(work + "/a"), "stale", &why));
    CHECK(fs::writeFile(work + "/a/after", "ok", 0644, &why) && read(work + "/a/after") == "ok");

    // 4. Copy and move: the mode is kept or set, the target is replaced, the source of a move is gone.
    CHECK(fs::copyFile(work + "/a/file", work + "/a/copy", -1, &why) && read(work + "/a/copy") == "second" && modeOf(work + "/a/copy") == 0755);
    CHECK(fs::copyFile(work + "/a/file", work + "/a/copy", 0600, &why) && modeOf(work + "/a/copy") == 0600);
    CHECK(!fs::copyFile(work + "/a/none", work + "/a/copy", -1, &why) && read(work + "/a/copy") == "second");
    CHECK(fs::moveFile(work + "/a/copy", work + "/a/b/moved", &why) && fs::kindOf(work + "/a/copy") == fs::kNone);
    CHECK(read(work + "/a/b/moved") == "second" && modeOf(work + "/a/b/moved") == 0600);
    CHECK(fs::moveFile(work + "/a/b/moved", work + "/a/file", &why) && read(work + "/a/file") == "second" && modeOf(work + "/a/file") == 0600);
    CHECK(!fs::moveFile(work + "/a/none", work + "/a/x", &why));

    // 5. Links are looked at, never through.
    CHECK(fs::makeLink("file", work + "/a/link", &why) && fs::kindOf(work + "/a/link") == fs::kLink);
    std::string target;
    CHECK(fs::readLink(work + "/a/link", &target) && target == "file" && !fs::readLink(work + "/a/file", &target));
    CHECK(fs::makeLink("b", work + "/a/dirlink", &why) && fs::kindOf(work + "/a/dirlink") == fs::kLink && fs::isDir(work + "/a/dirlink"));
    CHECK(fs::removeFile(work + "/a/link", &why) && fs::kindOf(work + "/a/file") == fs::kFile);
    CHECK(fs::removeFile(work + "/a/link", &why));                           // gone already: fine
    CHECK(fs::removeFile(work + "/a/file/below", &why));                     // a path through a file: nothing there
    CHECK(fs::removeTree(work + "/a/dirlink", &why) && fs::kindOf(work + "/a/b/c") == fs::kDir);   // the link, not b

    // 6. Listing, hashing, and removing.
    std::vector<std::string> names;
    CHECK(fs::listDir(work + "/a", &names, &why) && names.size() == 5);
    CHECK(names[0] == "after" && names[1] == "b" && names[2] == "file" && names[3] == "log" && names[4] == "one");
    CHECK(!fs::listDir(work + "/none", &names, &why));
    uint8_t digest[32];
    uint64_t size = 0;
    CHECK(fs::writeFile(work + "/a/abc", "abc", 0644, &why) && fs::hashFile(work + "/a/abc", digest, &size, &why) && size == 3);
    CHECK(toHex(digest, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(!fs::removeEmptyDir(work + "/a") && fs::removeEmptyDir(work + "/a/one") && fs::removeEmptyDir(work + "/a/one"));
    CHECK(fs::removeTree(work + "/a", &why) && fs::kindOf(work + "/a") == fs::kNone && fs::removeTree(work + "/a", &why));
    uint64_t free = 0;
    CHECK(fs::freeBytes(work + "/not/there/yet", &free) && free > 0 && fs::sameDevice(work, work + "/not/there"));

    // 7. Processes: output from both streams, the exit code, the environment and the working folder. A second
    // thread waits for the whole of it, as the threads of the car's libraries do in the agent.
    static pthread_mutex_t hold = PTHREAD_MUTEX_INITIALIZER;
    pthread_t other;
    pthread_mutex_lock(&hold);
    CHECK(pthread_create(&other, 0, &waitFor, &hold) == 0);
    std::string out;
    ProcessResult r = sh("echo out; echo err >&2; echo \"$NBT_TEST\"; pwd; echo \"[$HOME]\"; exit 3", work, 10, &out);
    CHECK(r.exited && r.exitCode == 3 && !r.timedOut);
    const std::string& cwd = work;
    CHECK_IN(out, out == "out\nerr\nvalue with spaces\n" + cwd + "\n[]\n");   // HOME is not passed on
    r = sh("exit 0", cwd, 10, &out);
    CHECK(r.exited && r.exitCode == 0 && out.empty());
    r = sh("kill -TERM $$", cwd, 10, &out);
    CHECK(!r.exited && !r.timedOut && r.signal == 15);
    // Standard input is empty.
    r = sh("read line; echo \"read: $?\"", cwd, 10, &out);
    CHECK(r.exited && out == "read: 1\n");
    // What the caller ignores is not ignored in the child. The agent ignores SIGPIPE, and SIGHUP on a head unit;
    // a script that inherited that could not be ended by either, and nor could anything it starts.
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    r = sh("kill -HUP $$; echo survived", cwd, 10, &out);
    CHECK_IN(out, !r.exited && r.signal == SIGHUP && out.empty());
    r = sh("kill -PIPE $$; echo survived", cwd, 10, &out);
    CHECK_IN(out, !r.exited && r.signal == SIGPIPE && out.empty());
    signal(SIGPIPE, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    // Output larger than a pipe holds arrives whole.
    r = sh("i=0; while [ $i -lt 20000 ]; do echo 0123456789; i=$((i+1)); done", cwd, 30, &out);
    CHECK(r.exited && r.exitCode == 0 && out.size() == 220000);

    // The timeout kills the script and what it started, and what was printed before still arrives.
    time_t before = time(0);
    r = sh("echo started; (sleep 30; echo late > survivor) & sleep 30; echo never", cwd, 1, &out);
    CHECK(r.timedOut && !r.exited && out == "started\n" && time(0) - before <= 5);
    sleep(1);
    CHECK(fs::kindOf(cwd + "/survivor") == fs::kNone);
    // A process left running on purpose does not hold the call up, and is not killed.
    before = time(0);
    r = sh("(sleep 2; echo late > survivor) & echo left one behind", cwd, 20, &out);
    CHECK(r.exited && r.exitCode == 0 && out == "left one behind\n" && time(0) - before <= 1);
    sleep(3);
    CHECK(read(cwd + "/survivor") == "late\n");
    // A program that does not exist, and a folder that does not exist.
    std::vector<std::string> args, env;
    args.push_back("/no/such/program");
    Collect none;
    CHECK(runProcess("/bin/sh", args, env, cwd, 5, &none, &r, &why) && r.exited && r.exitCode == 127);
    r = sh("echo x", cwd + "/no/such/folder", 5, &out);
    CHECK(r.exited && r.exitCode == 126 && out.empty());
    // A program named without a folder is the one in the working folder, never one found along PATH.
    CHECK(fs::writeFile(cwd + "/ls", "#!/bin/sh\necho \"mine: $1\"\n", 0755, &why));
    args[0] = "ls";
    args.push_back("-l *");
    env.push_back("PATH=/usr/bin:/bin");
    Collect mine;
    CHECK(runProcess("/bin/sh", args, env, cwd, 5, &mine, &r, &why) && r.exited && r.exitCode == 0);
    CHECK_IN(mine.text, mine.text == "mine: -l *\n");
    // No shell to start it with, and nothing to start.
    CHECK(!runProcess("/no/such/shell", args, env, cwd, 5, &none, &r, &why) && why.find("/no/such/shell") != std::string::npos);
    args.clear();
    CHECK(!runProcess("/bin/sh", args, env, cwd, 5, &none, &r, &why));
    // The caller's own open files are closed in the child: what it writes to one of their numbers goes nowhere.
    int mineOnly = open((cwd + "/mine-only").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(mineOnly >= 0 && dup2(mineOnly, 9) == 9);
    r = sh("echo leaked >&9", cwd, 5, &out);
    CHECK(write(9, "kept\n", 5) == 5 && close(9) == 0 && (mineOnly == 9 || close(mineOnly) == 0));
    CHECK_IN(read(cwd + "/mine-only"), read(cwd + "/mine-only") == "kept\n");
    // All of the above ran beside a second thread: starting a process must not depend on there being one only.
    pthread_mutex_unlock(&hold);
    CHECK(pthread_join(other, 0) == 0);

    // 8. The journal: lines come back in order; a last line with no newline is dropped; a second create fails.
    std::string log = cwd + "/journal";
    {
        Journal journal;
        CHECK(journal.create(log, &why) && journal.append("{\"op\":\"begin\"}", &why) && journal.append("{\"op\":\"place\"}", &why));
        Journal second;
        CHECK(!second.create(log, &why));
    }
    std::vector<std::string> lines;
    CHECK(Journal::read(log, &lines, &why) && lines.size() == 2 && lines[0] == "{\"op\":\"begin\"}" && lines[1] == "{\"op\":\"place\"}");
    CHECK(fs::appendFile(log, "{\"op\":\"comm", &why) && Journal::read(log, &lines, &why) && lines.size() == 2);
    CHECK(!Journal::read(log + ".none", &lines, &why));

    // 9. The JSON writer writes what the JSON reader reads.
    json::Writer w;
    w.beginObject().key("text").string("a\"b\\c\n\x01\xc3\xa9").key("n").integer(-42).key("big").integer(9007199254740991LL);
    w.key("yes").boolean(true).key("list").beginArray().integer(1).string("two").beginObject().endObject().beginArray().endArray().endArray();
    w.key("empty").beginObject().endObject().key("nested").beginObject().key("k").boolean(false).endObject().endObject();
    CHECK(w.text() == "{\"text\":\"a\\\"b\\\\c\\u000a\\u0001\xc3\xa9\",\"n\":-42,\"big\":9007199254740991,\"yes\":true,"
                      "\"list\":[1,\"two\",{},[]],\"empty\":{},\"nested\":{\"k\":false}}");
    json::Document doc;
    CHECK_IN(why, doc.parse(w.text(), &why));
    CHECK(doc.root().find("text").string() == "a\"b\\c\n\x01\xc3\xa9" && doc.root().find("n").integer() == -42);
    CHECK(doc.root().find("list").size() == 4 && !doc.root().find("nested").find("k").boolean());

    CHECK(fs::removeTree(work, &why));
    printf("test_fsutil: ok\n");
    return 0;
}

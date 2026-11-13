#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""The agent on the test bench, in a Docker container.

  tools/bench.py image                 build the image (quick when nothing has changed)
  tools/bench.py have                  exit status 0 if Docker and the image are there, 1 if not
  tools/bench.py agent ARGS...         nbtagent in a container; ARGS as the host build's nbtagent takes them
  tools/bench.py test NAME [ARGS...]   the test program NAME (such as test_engine) in a container
  tools/bench.py sh SCRIPT [ARGS...]   a shell script of this checkout in a container, under the image's sh
  tools/bench.py testdata OUTDIR       the test packages (tests/make_packages.py) built in a container, into OUTDIR

Why: a package's hook is an ordinary process of whoever runs the agent. --root says where the agent puts files; it
does not keep a script from the rest of the machine. In the container a hook sees a small Debian, the folders
named below, and nothing else of this machine, and it is not root there.

`agent` hands the container, at the paths they have here:
  the --root folder                 to write: made first if it is not there
  the folder of --socket            to write, when it is not inside the root: use one that holds nothing else
  each --test-key file              to read
With no --debug-tcp the container has no network at all, and the local socket is the only way in. With
--debug-tcp PORT, 127.0.0.1:PORT of this machine leads to the agent (0: a free port, which the agent's first lines
name), and the container is on Docker's default network: a hook can then reach whatever that network reaches.

`test` and `sh` run from the repo's top folder, which the container may read, and may write build/bench/work only:
a test that takes a work folder has to be given one in there. They are handed the NBT_TEST_... variables of this
program's environment. `sh` is for the agent's own shell scripts (agent/nbtlaunch.sh, agent/hu_install.sh), which
start programs as a package's hook does; the image's sh is Debian's dash, not the head unit's ksh.

Stopping: a SIGINT or SIGTERM to this program reaches the agent, which ends as it does on the host. If this program
is killed outright, the container goes on until the agent is done; `docker ps --filter label=nbtapp-bench` shows it.

`testdata` is for the copies of the test packages that the Android app's tests keep, and for the check that they
are current. Those are compared byte for byte, and the deflate data in a package is whatever the zlib that made it
makes: zlib and zlib-ng, say, make other bytes of the same files. So they are made with the zlib of one pinned
Debian, in an image of their own (the Dockerfile's stage `vectors`, named as the bench image with "-vectors"),
which may read this checkout and write OUTDIR only, and has no network. `make testdata` still builds them with this
machine's Python, for the agent's own tests, which compare them with nothing.

The image is nbtapp-bench, or what NBT_BENCH_IMAGE names. Needs docker, and for `image` git.
"""
import os, socket, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE = os.environ.get("NBT_BENCH_IMAGE", "nbtapp-bench")
VECTORS_IMAGE = IMAGE + "-vectors"
DOCKERFILE = os.path.join(REPO, "tools", "bench", "Dockerfile")
PROGRAMS = "/opt/nbtagent"
WORK = os.path.join(REPO, "build", "bench", "work")


def fail(message, status=2):
    print("bench.py: " + message, file=sys.stderr)
    sys.exit(status)


def image():
    try:
        version = subprocess.run(["git", "-C", REPO, "describe", "--always", "--dirty"], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL).stdout.decode().strip() or "unknown"
    except OSError:
        version = "unknown"
    print("bench.py: building the image %s (minutes the first time, a moment when nothing has changed)" % IMAGE,
          file=sys.stderr, flush=True)
    os.execvp("docker", ["docker", "build", "--quiet", "--provenance=false", "--tag", IMAGE, "--build-arg", "VERSION=" + version,
                         "--file", DOCKERFILE, REPO])


def have():
    try:
        found = subprocess.run(["docker", "image", "inspect", IMAGE], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        sys.exit(1)
    sys.exit(0 if found.returncode == 0 else 1)


def container(kind, options, command, image=IMAGE):
    """Replaces this program with `docker run`: what the container prints, its exit status and the signals sent
    here are then the container's."""
    # With this machine's time zone, so that the times in the agent's log are the ones on the wall here.
    zone = os.path.realpath("/etc/localtime")
    if os.path.isfile(zone) and "," not in zone:
        options = ["--mount", "type=bind,source=%s,target=/etc/localtime,readonly" % zone] + options
    os.execvp("docker", [
        "docker", "run", "--rm",
        "--init",                                   # reaps what a hook leaves behind, and passes signals on
        "--user", "%d:%d" % (os.getuid(), os.getgid()),
        "--read-only", "--tmpfs", "/tmp",
        "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--pids-limit", "512",
        "--label", "nbtapp-bench=" + kind,
    ] + options + [image] + command)


def bind(path, mode="rw"):
    if "," in path or ":" in path:
        fail("%s: a path handed to Docker cannot hold a comma or a colon" % path)
    return ["--mount", "type=bind,source=%s,target=%s%s" % (path, path, ",readonly" if mode == "ro" else "")]


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def inside(path, folder):
    return path == folder or path.startswith(folder.rstrip("/") + "/")


def agent(args):
    root = socket_path = port = None
    keys, passed = [], []
    at = 0
    while at < len(args):
        name = args[at]
        if name == "--version":
            passed.append(name)
            at += 1
            continue
        if name not in ("--root", "--socket", "--test-key", "--debug-tcp") or at + 1 == len(args):
            fail("agent takes --root DIR [--socket PATH] [--test-key FILE]... [--debug-tcp PORT]")
        value = args[at + 1]
        at += 2
        if name == "--debug-tcp":
            if not value.isdigit() or int(value) > 65535:
                fail("--debug-tcp takes a port, 0 for any free one")
            port = int(value) or free_port()
            continue
        value = os.path.abspath(value)
        if name == "--root":
            root = value
        elif name == "--socket":
            socket_path = value
        else:
            if not os.path.isfile(value):
                fail("%s is not a file" % value)
            keys.append(value)
        passed += [name, value]
    if root is None and passed != ["--version"]:
        fail("agent needs --root DIR")

    options = []
    if root is not None:
        # Made here: a folder Docker has to make itself would belong to root.
        os.makedirs(root, exist_ok=True)
        options += bind(root)
        if socket_path is not None and not inside(socket_path, root):
            folder = os.path.dirname(socket_path)
            os.makedirs(folder, exist_ok=True)
            options += bind(folder)
    for key in keys:
        options += bind(key, "ro")
    if port is None:
        options += ["--network", "none"]
    else:
        # Published on this machine's loopback address only. Inside, the agent has to listen on the address the
        # published port arrives at, which is not the container's own 127.0.0.1.
        options += ["--publish", "127.0.0.1:%d:%d" % (port, port)]
        passed += ["--debug-tcp", "0.0.0.0:%d" % port]
        print("bench.py: the agent's debug TCP link is 127.0.0.1:%d on this machine" % port, file=sys.stderr, flush=True)
    container("agent", options, [PROGRAMS + "/nbtagent"] + passed)


def in_repo():
    """The options of a container that works in this checkout: it may read it and write build/bench/work."""
    os.makedirs(WORK, exist_ok=True)
    options = bind(REPO, "ro") + bind(WORK) + ["--workdir", REPO, "--network", "none"]
    for name in sorted(os.environ):
        if name.startswith("NBT_TEST_"):
            options += ["--env", name]
    return options


def test(args):
    if not args or "/" in args[0] or not args[0].startswith("test_"):
        fail("test takes the name of a test program, such as test_engine, and its arguments")
    container("test", in_repo(), [PROGRAMS + "/" + args[0]] + args[1:])


def shell(args):
    script = os.path.abspath(args[0]) if args else ""
    if not os.path.isfile(script) or not inside(script, REPO):
        fail("sh takes a shell script of this checkout, and its arguments")
    container("sh", in_repo(), ["/bin/sh", script] + args[1:])


def testdata(args):
    if len(args) != 1:
        fail("testdata takes the folder to build the test packages into")
    out = os.path.abspath(args[0])
    # Built each time, as `image` is: quick when nothing has changed. Its build prints only what went wrong.
    built = subprocess.run(["docker", "build", "--quiet", "--provenance=false", "--target", "vectors",
                            "--tag", VECTORS_IMAGE, "--file", DOCKERFILE, REPO], stdout=subprocess.DEVNULL)
    if built.returncode != 0:
        fail("could not build the image %s" % VECTORS_IMAGE, built.returncode)
    # Made here: a folder Docker has to make itself would belong to root.
    os.makedirs(out, exist_ok=True)
    options = bind(REPO, "ro") + bind(out) + ["--workdir", REPO, "--network", "none", "--env", "PYTHONDONTWRITEBYTECODE=1"]
    container("testdata", options, ["python3", os.path.join(REPO, "tests", "make_packages.py"), out], VECTORS_IMAGE)


def main():
    commands = {"image": image, "have": have}
    with_args = {"agent": agent, "test": test, "sh": shell, "testdata": testdata}
    args = sys.argv[1:]
    try:
        if len(args) == 1 and args[0] in commands:
            commands[args[0]]()
        elif args and args[0] in with_args:
            with_args[args[0]](args[1:])
        else:
            print(__doc__, file=sys.stderr)
            sys.exit(2)
    except FileNotFoundError:
        fail("docker is not installed, or not on the PATH", 1)


if __name__ == "__main__":
    main()

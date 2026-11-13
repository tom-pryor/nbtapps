# NBTApp

A native iDrive app for BMW NBT Evo head units (iDrive 5/6, QNX Neutrino 6.5.0 SP1, ARMv7LE). For now it is
small: an **NBTApp** entry in Media/Radio that opens a menu with four rows, **Start the agent**, **Pair a phone**,
**Forget all phones** and **About**. The first starts the install agent on a head unit with no console, and the
next two are the car's side of pairing a phone app with it (see [Packages and the agent](#packages-and-the-agent)).

It is an iDrive **PluginApp**, built on the **HU Framework**: the head unit's own framework for add-on apps such as
`SecurityMenu` and `ActiveSeating` (`libAppFramework.A.so`, `libCoreFramework.A.so`). At boot, HmiMain3D loads every
`Plugins/<Name>/<Name>.manifest` under `/opt/hmi/ID5/data/ro`. The entry button and its placement come from the XML,
so iDrive creates the menu row itself. Nothing in HmiMain3D is patched.

**Status: builds, never run on a car.**

## Layout

| Path | Purpose |
|---|---|
| `src/nbt_app.cpp` | The app (`pluginAppInfo` entry point). Fills the About text and drives the three screens that talk to the agent; the menu and its steps are XML |
| `src/agent_start.hpp` | The start of the agent's launcher when the HMI starts, on a head unit that is asked to start the agent by itself, and the record that keeps an agent which brings the head unit down from being started for ever; and its start by hand from the Start the agent screen |
| `src/phone_screens.hpp`, `src/agent_link.hpp` | What the Start the agent, Pair a phone and Forget all phones screens ask of the agent, and when; and their link to it, the agent's local socket |
| `src/hu_framework.hpp` | Declarations of the HU Framework (it ships no headers) |
| `src/version.hpp` | The plugin's version, which its package is released as |
| `plugin/NbtApp.manifest` | Tells iDrive what to load |
| `plugin/NbtApp.xml` | The UI: entry in Media/Radio after the Bluetooth audio row, menu (`hmiState` 10), Start the agent (`hmiState` 16), Pair a phone (`hmiState` 14), Forget all phones (`hmiState` 15), About page (`hmiState` 12), preview (`hmiState` 11) |
| `assets/` | Images: 63x54 row icon, 549x484 preview |
| `stubs/*.syms` | Symbol names of the two HU Framework libraries. The build makes empty link stubs from them, so no head-unit binary is needed |
| `tools/make_db.py` | The texts and image ids; packs the text and image DBs |
| `plugin/package/manifest.json.in` | The manifest of the plugin's own package, `org.nbtapp.plugin` |
| `tools/plugin_pkg.py` | Builds that package of `release/NbtApp/` (`make plugin-pkg`) and signs it for a release (`make plugin-release`) |
| `tools/check.py` | `make check` |
| `tools/make_icons.py`, `tools/update_stubs.sh` | Redraw the images; regenerate `stubs/*.syms` from a firmware dump |
| `agent/` | The install agent: the package reader (zip profile, signature, manifest, hashes), the install engine (install, upgrade, remove, rollback, recovery), and the protocol a phone speaks to it (frames, messages, plans, jobs), on SHA-256, a strict JSON parser, inflate, and Ed25519 verification from `agent/vendor/tweetnacl`. Two programs: `nbtagent`, which runs on a folder that stands in for the head unit and, in the build for the head unit, has a mode for the head unit itself that does not start yet, and `nbtctl`, a command-line controller. Also here is the Bluetooth serial link, which only the head unit's build of `nbtagent` offers: `dsi_client` (the head unit's service-to-service IPC), `spp_service` (the registration of a serial service of our own with the Bluetooth stack), `spp_gate` (the gate in front of `/dev/btspp0`), `spp_nodes` (which serial node holds a link of ours), `serial_link` (the listener, and a link on a serial node), `programs` (which programs are running) and `serial_services` (the serial services other programs register, and the removal of one whose program is gone). `agent_home` is the agent's own folder on the head unit, a folder to each version of it, which the agent updates itself through. Four scripts for the head unit's shell: `hu_probe.sh` reads off a head unit what the agent has to know about it, `hu_install.sh` puts the agent onto one, `nbtstart.sh` is the starter that decides there which version of the agent is started, and `nbtlaunch.sh` is a version's own launcher, which starts that version's `nbtagent` and starts it again when it fails |
| `spec/` | Definitions shared with the Android app: `manifest.schema.json`; `json-profile.vectors`, the cases every JSON parser of the project must agree on; and `protocol/`, the frames every codec must build and refuse alike, one whole conversation with the agent, frame by frame, and `constants.txt`, the names and numbers both sides must hold alike |
| `tools/nbtpkg.py` | Builds, signs and verifies `.nbtpkg` packages |
| `tools/check_agent.py` | `make agent-check`: `release/agent/` is the head unit's kind of program, holds nothing of the test bench, and its `nbtagent` holds the official package key; and the agent's own package in `release/pkg/` is byte for byte what `tools/agent_pkg.py` makes of that folder |
| `agent/package/manifest.json.in`, `tools/agent_pkg.py` | The agent's own package, `org.nbtapp.agent`: its manifest, with a placeholder for the version, and the script that makes the archive of `release/agent/` (`make agent-pkg`) and signs it for a release (`make agent-release KEY=FILE`); `tests/test_agent_pkg.py` tests the script |
| `tests/` | Host tests of the Python tools: `test_nbtpkg.py` (`make test-pkg`), `test_agent_pkg.py` (`make test-agent-pkg`) and `test_plugin_pkg.py` (`make test-plugin-pkg`) |
| `tests/agent/` | Tests of `agent/` (`make agent-test`), run in a Docker container (`tools/bench.py`); `tests/compare_readers.py` holds the agent's package reader and the tool's together, `tests/test_protocol_vectors.py` reads `spec/protocol/` with a codec of its own, `tests/e2e/run_agent.py` runs `nbtagent` and `nbtctl` as two processes, and `tests/e2e/run_launcher.py` runs the starter, a version's launcher and the install script on folders |
| `tests/packages/`, `tests/keys/` | Package sources and test keys for `tests/make_packages.py`, which builds good and deliberately broken packages (`make testdata`); `tests/test_nbtpkg.py` tests the tool against them (`make test-pkg`) |
| `build/`, `release/` | Build products, not tracked. `release/NbtApp/` is the folder for the head unit; `release/agent/` holds the agent's programs for it, and `release/pkg/` the plugin's and the agent's own packages |

Ids that must not collide with other apps on the car: the manifest's connector id (211; BMW's apps use 17 and 200,
and 210 is known to be taken by another add-on app) and `entryButtonFocusID` (32754; 32752 and 32753 are taken the
same way). Text, image, model, action and state ids are local to this app. They are repeated across `plugin/NbtApp.xml`, `tools/make_db.py` and constants in `src/`;
`make check` verifies they agree.

## Building

The library needs the QNX SDP 6.5.0 (qcc, GCC 4.4.2); the rest needs python3 on the host. With the SDP in a Docker
image:

```sh
QNX_LICENSE_DIR=/path/to/qnx/license ./build.sh        # -> release/NbtApp/ and its package in release/pkg/, then make check
```

`build.sh` runs `make lib` in the image, then `make pkg`, `make plugin-pkg` and `make check` on the host. In an SDP
shell that also has python3, plain `make` does the first two.

The same source gives the same files, byte for byte. The library's log lines name the plugin's version
(`src/version.hpp`) and the build, which is `git describe --always --dirty`.

`make check` needs no car and no SDP. It checks that every id and reference in the XML, the DBs and the source
resolves, that the built library passes the head unit's ABI gate (ELF32 ARM, EABI5, soft-float calls, soname,
dependencies, `pluginAppInfo` exported, only HU Framework symbols that exist) and says it is the version in
`src/version.hpp`, and that the package in `release/pkg/` is the one of that folder.

To link against a head unit's real HU Framework libraries instead of the stubs:
`make lib FRAMEWORK_LIBDIR=/path/to/opt/hmi/ID5/lib`. The result is identical.

## Packages and the agent

The agent installs signed `.nbtpkg` packages that a controller offers it. It reads and verifies a package, and
installs, upgrades and removes it with backups, rollback and recovery after a power cut; it plans a job from targets
and offers, pulls the archives it needs over the link, and reports as it goes. The plan document sets the package
format and the protocol.

It has not run on a car:

- With `--root`, `nbtagent` keeps everything under that folder, which stands in for the head unit. The code for a
  real one is `agent/hu_platform.cpp`: it remounts the system partition for a write and puts it back, and
  restarts the HMI, by running commands from a configuration. The bench tests it against a folder with scripts in
  place of those commands.
- The build for the head unit (`./build.sh agent`) uses that code when `nbtagent` is started with no `--root`: it
  then works on the head unit itself, with its state in `/var/opt/nbtapp` and its socket at `/tmp/nbtagent.sock`,
  where that build's `nbtctl` looks when it is told nothing else. The configuration for a car is
  `HeadUnitConfig::car()`: `${app}` is `/var/opt/nbtapp/apps/<id>`, the system partition is `/fs/sda0`, remounted
  with `/bin/mount -u -o rw /fs/sda0` and put back with `-o ro`, and the HMI is restarted with
  `/opt/core/bin/OnOffDSICommander appreset`. The file says where each value comes from. The two remount commands
  have not run on a car in that form; the agent asks the file system after each whether it is read-only, so one
  that does nothing fails the install step before a file is written, or leaves the partition writable until the
  head unit is turned off and says so in the install log. No command for a reboot is known: the agent answers a
  request for one with `unsupported`, and the head unit has to be turned off and on by hand. On a machine that has no `/fs/sda0` or no plugin folder this mode ends without
  making anything. The host's build has no such mode.
- The official package key is built in (`agent/embedded_keys.hpp`): the public half of the key that signs the
  official catalogue's packages. An agent built for the head unit trusts that key and no other. The host build
  also takes `--test-key` for the test keys.
- The signature an install checks is the one the plan checked: the `signature` of the controller's offer, over the
  manifest. For a package the official catalogue built, that is the archive's own `manifest.sig`. For a package
  its author built and signed with a key of their own, the catalogue gives a detached signature over the same
  manifest, signed with the official key, and the archive's `manifest.sig` is not read (`Package::open` with a
  signature; `nbtpkg.py verify --signature`). Such a package is the author's archive byte for byte, at the
  author's address. Agents from 1.2.0 on list the capability `detached-signature` in `hello`; an older agent plans
  such an offer and then refuses it at install with `bad-signature`. `nbtctl install --from` offers each archive
  with its own `manifest.sig`, so it does not install such a package.
- The links are a local socket; in the head unit's build, Bluetooth serial, which has not run on a car (see
  below); and in the host build only, a TCP port on the loopback address (`--debug-tcp`), for the phone app's
  debug build on an emulator. TCP for a car, with the frame MAC it needs, is still to come.
- A phone is paired with the agent before the agent does anything for it. On Bluetooth serial, and on the
  bench's TCP port, the agent answers `hello`, `ping`, `auth` and `pairing.issue`, and refuses everything else
  until the phone has proved with `auth` that it holds a pairing key. A phone gets its key for a code of six
  digits that is asked for on the head unit itself, over the local socket: the plugin's **Pair a phone** screen
  shows it, and `nbtctl pair` prints it. The code is good for two minutes; the third wrong one closes the window,
  and no new one opens for 30 seconds. The agent keeps up to 16 pairings in `phones.json` in its state folder,
  which only its own user can read. `nbtctl phones` lists them; `nbtctl forget PHONEID` and `nbtctl forget --all`
  end them, as the plugin's **Forget all phones** screen does, and a forgotten phone's links are closed. The
  local socket asks for none of this: whoever can open it is one of the head unit's own programs. The plan
  document has the reasons ("Authentication", "Enrolment on the car's screen") and the messages (its Message
  reference tab). The plugin's two screens have run on a build host only.
- On a Bluetooth serial link the agent gives 4,096 bytes as its largest frame, sends `ping` after 10 seconds with
  nothing heard, and ends the link after 30; a serial link with no `hello` 30 seconds after it opened is ended
  too. A serial link on which the protocol's magic bytes have not come 500 ms after the agent opened it is not
  the agent's, and is let go. Those numbers, with the serial service's name and UUID, are in
  `spec/protocol/constants.txt`, which the phone app has to keep to as well. They are first values: no run on a
  car has set them.
- The phone made a link, and the phone ends it. When the agent ends a link of its own accord it says so first,
  with the event `link.closing`, and closes its end once that is written out; nothing follows the event. Its
  `reason` is `stopping` (the agent's process ends), `restarting` (it ends for a newer version of itself, which
  the launcher starts), `forgotten` (the phone's pairing was ended on the head unit), `replaced` (a newer link
  from the same phone took this one's place) or `silent` (the phone answered neither the pings nor a
  `blob.fetch`). A phone app closes its side as soon as the event arrives. The local socket's links get no such
  event, and neither does a link that ends for what the phone did wrong: the error in the answer says why. Why
  this matters on Bluetooth serial is under "How a link ends" below.
- A controller can pull a package's log files, so that a user can send them to whoever asked. A package names
  them in its manifest's `logs`: files or a folder with a wildcard, in `${app}`, `${data}`, `${tmp}` (the
  package's own folder on the RAM disk, `/tmp/nbtapp/apps/<id>` on a car, which the agent makes for it and a
  power down empties; hooks get it as `NBT_TMP_DIR`) or under `/tmp` and `/var` outside the agent's folders. The
  agent's own logs are those of `org.nbtapp.agent`. `files.list` lists them, regular files only and none reached
  through a link, and `files.fetch` sends one as a raw deflate stream (or as it is), a frame at a time while the
  link keeps up; an agent from 1.3.0 has these and lists the capability `files` in `hello`. `nbtctl logs ID`
  lists them, and `nbtctl pull ID PATH --out FILE` fetches one (`agent/log_files.hpp`, and the plan document's
  Message reference).

`agent/hu_probe.sh` collects those answers from a head unit: which node it is, what is mounted where and what is
read-only, the room in `/var/opt` and `/tmp`, the plugin folders and their manifests, the tools a hook can count on,
the usage text of the program that restarts the HMI, and where the shared libraries are that the agent's programs
ask for. Run with no argument it changes nothing; `--exec-test` is a second run that copies a small stock program
into `/tmp` and `/var/opt`, runs the copy and removes it, to see whether programs start from there. Its header says
what it reads. It has not run on a car. The commands are under [The agent on the head unit](#the-agent-on-the-head-unit).

On the bench the agent runs in a Docker container. A package's hook script is an ordinary process of whoever runs
the agent: `--root` says where the agent puts files, and does not keep a script from anything else that user can
reach. `tools/bench.py agent` takes `nbtagent`'s arguments and starts it in a container that is handed the root
folder, the folder of the socket and the test key, and nothing else of this machine: a small Debian with its root
file system read-only, no compiler, not root, and no network unless `--debug-tcp` is given. `make agent-test` and
the Android repo's tests against the real agent use it. (On a car a hook runs as root with nothing in its way; the
container protects the bench, it does not imitate the head unit.)

```sh
python3 tools/nbtpkg.py keygen mykey                       # mykey.key (private) and mykey.pub
python3 tools/nbtpkg.py build tests/packages/hello-1.2.0   # -> org.nbtapp.hello-1.2.0.nbtpkg, not signed
python3 tools/nbtpkg.py sign org.nbtapp.hello-1.2.0.nbtpkg --key mykey.key
python3 tools/nbtpkg.py verify org.nbtapp.hello-1.2.0.nbtpkg --key mykey.pub
```

`nbtpkg.py` needs python3 with the `cryptography` module; its header has the details. The keys in `tests/keys/` are
for tests only: their private halves are in this repo.

To try the agent by hand, in a scratch folder:

```sh
make bench-image agent-host testdata                       # the agent's image; build/agent-host/nbtctl; the test packages
mkdir -p /tmp/hu /tmp/stick && cp build/testdata/good/org.nbtapp.runtime-2.1.0.nbtpkg \
    build/testdata/good/org.nbtapp.hello-1.2.0.nbtpkg /tmp/stick/
python3 tools/bench.py agent --root /tmp/hu --test-key tests/keys/test-package.pub &
export NBT_AGENT_SOCKET=/tmp/hu/agent.sock
build/agent-host/nbtctl plan --from /tmp/stick org.nbtapp.hello      # what a job would do
build/agent-host/nbtctl install --from /tmp/stick org.nbtapp.hello   # the runtime it depends on comes along
build/agent-host/nbtctl list
build/agent-host/nbtctl remove org.nbtapp.hello org.nbtapp.runtime
```

`nbtctl` speaks the protocol a phone will speak and serves the packages of a folder when the agent asks for them;
its header lists the other commands. It runs on this machine and reaches the agent through the socket in the root
folder. `make agent-host` also builds an `nbtagent` for this machine, which takes the same arguments and keeps no
script from anything: leave it for when Docker is not to be had. After a change to `agent/`, `make bench-image`
again; `make agent-test` does that itself.

The agent writes what it does to standard error, a line at a time with the time of day: links that come and go,
each request and how it was answered, the plans it makes, what it fetches, and every step, phase and log line of a
job, the hooks' output included. Started as above, that is the terminal it was started in:

```
17:45:33.313 nbtagent: link 2 opened (local)
17:45:33.315 nbtagent: link 2: job.plan asks to install org.nbtapp.hello 1.3.0; 1 package on offer
17:45:33.317 nbtagent: plan cf8dbb2b757fe142: 1 step, 1824 bytes to fetch, restart: none
17:45:33.317 nbtagent:   upgrade org.nbtapp.hello 1.3.0 (from 1.2.0)
17:45:33.317 nbtagent: link 2: job.plan -> ok (2 ms)
17:45:33.322 nbtagent: job, step 1: postInstall| post-install: failing on purpose
17:45:33.327 nbtagent: job cf8dbb2b757fe142 failed, restart needed: none
17:45:33.327 nbtagent:   upgrade org.nbtapp.hello 1.3.0: failed (hook-failed: postInstall exited with 3)
```

To try it from the phone app (the debug build of the Android repo, on an emulator or on a phone on USB):

```sh
python3 tools/bench.py agent --root /tmp/hu --test-key tests/keys/test-package.pub --debug-tcp 7144 &
adb reverse tcp:7144 tcp:7144                              # the device's 127.0.0.1:7144 is now this machine's
```

Then add a head unit at `127.0.0.1`, port `7144`, in the app, and connect. The app asks for a pairing code, as it
does on a car. Here `nbtctl` stands in for the car's screen:

```sh
NBT_AGENT_SOCKET=/tmp/hu/agent.sock build/agent-host/nbtctl pair    # prints "code NNNNNN", then "paired"
```

Type the code into the app, and install from its sample catalogue, which is signed with the same test key. The
port is published on this machine's 127.0.0.1 only, and a program that connects to it has to be paired as the app
is. A pairing key crosses it in the clear, though, and a program that can open the socket in the root folder
needs no key: it can have the agent install a package and run its hooks.
Those run in the container, where they can change the root folder and nothing else here; with `--debug-tcp` the
container is on Docker's default network, so a hook can reach what that network reaches. Stop the agent when you
are done.

```sh
make test-pkg                                              # the tool and the test packages
make agent-test                                            # agent/ in the bench container (docker); about a minute
QNX_LICENSE_DIR=/path/to/qnx/license ./build.sh agent      # those tests, then built with qcc -> release/agent/ and release/pkg/, then make agent-check
make agent-pkg                                             # the agent's own package from release/agent/ -> release/pkg/, not signed
make agent-release KEY=/path/to/package.key                # that package and the check again, then signed -> release/pkg/signed/
make build/agent-host/test_package testdata                # the agent's reader for this machine, which the next line needs
python3 tests/compare_readers.py                           # the tool's reader against the agent's, on changed packages
NBT_TEST_ALL_PAIRS=1 python3 tools/bench.py test test_engine build/testdata build/bench/work   # the engine's long crash test
```

The golden files in `spec/protocol/` are written by two of the test programs. The container may write
`build/bench/work` only, so they are written there and copied:

```sh
python3 tools/bench.py test test_frame --write build/bench/work/frames.vectors     # after a change to the frame cases
python3 tools/bench.py test test_agent --write-session build/bench/work/session.vectors \
    build/testdata build/bench/work/agent                                          # after a change to what the agent says
cp build/bench/work/frames.vectors build/bench/work/session.vectors spec/protocol/
```

`./build.sh agent` leaves the head-unit builds of the library, the test programs, `nbtagent` and `nbtctl` in
`build/agent/`, and `nbtagent`, `nbtctl`, the file `version` that names their version, and the scripts
`hu_probe.sh`, `hu_install.sh`, `nbtlaunch.sh` and `nbtstart.sh` in `release/agent/`, the folder to take to a head unit.
`make agent-check` then looks at that folder: both programs are ARM executables of the kind the head unit starts
(EABI5, soft-float calls, not position-independent) that ask for `libsocket.so.3`, `libcpp.so.4` and `libc.so.3`
only, neither holds the test bench's options or a key from `tests/keys/`, `nbtagent` holds the package key of
`agent/embedded_keys.hpp`, the scripts are the ones in `agent/`, and `version` is the agent's. The build also
makes the agent's own package of that folder, `release/pkg/org.nbtapp.agent-<version>.nbtpkg`, not signed, and
`make agent-check` holds the two together: the archive is byte for byte what `tools/agent_pkg.py` makes of the
folder, and its manifest places the two programs, the launcher and the starter and does nothing else. None of it
has run on a car. The
tests end the agent at every point of an install step and check what the next start makes of it; they cannot
simulate a real power cut, which can also lose writes that were not synced.

## Installing

[INSTALL.md](INSTALL.md) sets up a car and a phone from nothing, step by step: the agent, the app and this
plugin. This section is the plugin's own detail.

There are two ways. A head unit that has the agent (version 1.1.0 or later) takes the plugin as a package, from
the phone app, like any other: "The plugin's package", below. Without an agent it is copied by hand.

### The plugin's package

`org.nbtapp.plugin` is the plugin as a package of its own (`tools/plugin_pkg.py` makes it). It places the five
files of `release/NbtApp/` as `${plugins}/NbtApp/`, which on a head unit is `/opt/hmi/ID5/data/ro/Plugins/NbtApp`,
and asks for a restart of the HMI, which loads a plugin only when it starts. The agent installs, updates and
removes it with the step every package gets: the system partition is writable only while files are placed, a
library that is loaded is moved out of `Plugins` and never written over, and an update that fails is undone.

```sh
QNX_LICENSE_DIR=/path/to/qnx/license ./build.sh        # also leaves release/pkg/org.nbtapp.plugin-<version>.nbtpkg, not signed
make plugin-release KEY=/path/to/package.key           # -> release/pkg/signed/, and the SHA-256 the catalogue records
```

- **Its version** is the plugin's own, in `src/version.hpp`, and is not the agent's. A version that has been
  released is never built again with other content: raise the number.
- **`requires.agent`** in `plugin/package/manifest.json.in` names the oldest agent the plugin runs with. It is
  1.1.0, the first agent whose update brings its launcher: the plugin starts that launcher's starter. When it is
  raised, and in which order the two packages are released, is under
  [Releasing the agent and the plugin](#releasing-the-agent-and-the-plugin).
- **`make plugin-release`** refuses what `make agent-release` refuses of the agent's package: a library that was
  not built from the commit checked out, or does not say it is the version of the package, and a key that is not
  built into the agent.
- **A plugin that was copied onto a head unit by hand** is in the way: the agent writes over no file that it has
  no record of, and the install fails with `stock-mismatch`. Move that folder out of `Plugins` once, as shown
  below for an update by hand, and install the package.

The bench installs, updates and removes this package as `tools/plugin_pkg.py` makes it, with stand-ins for the
library and the DBs (`tests/agent/test_engine.cpp`). The agent has not installed it on a car.

### By hand

Over SSH on the head unit, with `release/NbtApp/` on a USB stick:

```sh
mount -o remount rw /net/hu-omap/fs/sda0
on -f /net/hu-omap/ /bin/sh
ls /mnt                                   # find the stick: umassXXXXX
mkdir -p /opt/hmi/ID5/data/ro/Plugins/NbtApp
cp /mnt/umassXXXXX/NbtApp/* /opt/hmi/ID5/data/ro/Plugins/NbtApp/
ls -l /opt/hmi/ID5/data/ro/Plugins/NbtApp/   # five files
OnOffDSICommander appreset                # restarts the HMI; or hold the power button 30 s
```

To update, never copy over the loaded library: move the old folder out of `Plugins` first (a copy left inside
`Plugins` could be loaded as a second app), then install as above.

```sh
mv /opt/hmi/ID5/data/ro/Plugins/NbtApp /opt/NbtApp.old
```

To switch the app off without a remount: `mkdir -p /var/opt/nbtapp && touch /var/opt/nbtapp/disable`, then restart
the HMI. The app then does not start the agent either (see "Starting with the head unit"). To remove it, delete the
`Plugins/NbtApp` folder.

Log: `sloginfo | grep NbtApp`.

## The agent on the head unit

`release/agent/` holds `nbtagent`, `nbtctl`, `version` and the scripts `hu_probe.sh`, `hu_install.sh`,
`nbtlaunch.sh` and `nbtstart.sh`. None of them has run on a car, so this is what the repo knows, and the probe's log is what settles
the rest:

- A USB stick is `noexec`, so the programs are copied off it before they are started. The lines below copy them to
  `/tmp`, which is emptied when the head unit powers down. Whether a program starts from `/tmp` or from `/var/opt`
  is what the probe's `--exec-test` run answers.
- The programs ask for `libsocket.so.3`, `libcpp.so.4` and `libc.so.3`, as the Bluetooth tools do. Whether a head
  unit has `libcpp.so.4` is not recorded here; the probe lists it, and `nbtagent --version` shows it at once.
- The agent installs a package that the official package key has signed, and no other. Nothing starts it when
  the head unit starts, until the iDrive plugin is asked to (see "Starting with the head unit").

Over SSH on the head unit, with the folder on a USB stick as `agent/`. First the probe, which changes nothing, and
its second run, which writes and removes one small file in `/tmp` and in `/var/opt`:

```sh
on -f /net/hu-omap/ /bin/sh                   # the node the HMI runs on
ls /mnt                                       # find the stick: umassXXXXX
S=/mnt/umassXXXXX/agent
ksh $S/hu_probe.sh > $S/hu_probe.log 2>&1
ksh $S/hu_probe.sh --exec-test >> $S/hu_probe.log 2>&1
```

Then the agent on a folder. With `--root` it keeps to that folder, remounts nothing and restarts nothing, here as
on a build host:

```sh
mkdir -p /tmp/nbt
cp $S/nbtagent $S/nbtctl /tmp/nbt/
chmod 755 /tmp/nbt/nbtagent /tmp/nbt/nbtctl
/tmp/nbt/nbtagent --version                   # does the program start here at all
/tmp/nbt/nbtagent --root /tmp/hu 2> /tmp/hu.log &
NBT_AGENT_SOCKET=/tmp/hu/agent.sock /tmp/nbt/nbtctl info
kill $!                                       # SIGTERM: the agent ends between two install steps
cat /tmp/hu.log
```

With no `--root`, `nbtagent` works on the head unit itself: state in `/var/opt/nbtapp`, the system partition
remounted for a write and put back, the HMI restarted. The archives it fetches and the packages it unpacks go to
`/var/opt/nbtapp/staging`, because the RAM disk behind `/tmp` is too small for them; it empties that folder the
first time it starts after a power down, so a fetch never goes on from before one. `/tmp/nbtapp` holds a few empty
marks that have to go when the head unit powers down, and nothing else. That partition also holds the head unit's
own settings, so the agent refuses a job that would leave less than 32 MB free on it. A package installed this
way is installed for real: its files are on the head unit after a power down, and a plugin is loaded by the HMI.

```sh
/tmp/nbt/nbtagent 2> /tmp/nbtagent.log &      # the socket is /tmp/nbtagent.sock, and only one agent gets it
/tmp/nbt/nbtctl info                          # this build's nbtctl looks there when it is told nothing else
```

In the head unit's build the agent also takes phone apps' links over Bluetooth serial: by default with no `--root`,
and with `--root` when it is given `--bluetooth` (`--no-bluetooth` turns it off). It registers a serial service
with the car's Bluetooth stack and puts the gate in front of `/dev/btspp0` (`agent/spp_gate.hpp`: a resource manager
attached at the same path, ahead of the Bluetooth stack's node, that passes an open on to the node or refuses the
car's media process; no data goes through it), and takes both away again when it stops; nothing of it is written to disk. While the agent runs with it, the car
maker's phone apps do not work over Bluetooth. If it cannot be set up, the agent says why in its log and goes on
with the local socket. If the car's Bluetooth service does not take the registration, or the service is not
registered for 20 seconds (an agent that starts with the head unit may be there before the car's Bluetooth
service is), the agent takes the gate away again, and sets both up anew 30 seconds later, and so on until the
service is registered. Its lines in the log start with `bluetooth:`. The phone's Bluetooth pairing with
the car is not enough on this link: until the phone has been paired with the agent and has proved it with `auth`,
the agent answers `hello`, `ping`, `auth` and `pairing.issue`, and refuses everything else. A link that does not open with the protocol's magic bytes is let go (see "Other programs' links"
below). One that does, and then does not say `hello` within 30 seconds or goes silent for as long, is ended. With
`--root` and `--bluetooth` a dropped SSH session stops the agent as `kill` does, so that the registration does
not stay behind. It has not run on a car:

```sh
/tmp/nbt/nbtagent --root /tmp/hu --bluetooth 2> /tmp/hu.log &
/tmp/nbt/nbtagent --root /tmp/hu --bluetooth --decide 800 --decide-hold 2> /tmp/hu.log &   # see "Other programs' links"
```

The phone app then asks for a pairing code. The plugin's **Pair a phone** screen shows one; so does `nbtctl`,
which needs nothing of the HMI:

```sh
NBT_AGENT_SOCKET=/tmp/hu/agent.sock /tmp/nbt/nbtctl pair            # prints "code NNNNNN", then "paired"
NBT_AGENT_SOCKET=/tmp/hu/agent.sock /tmp/nbt/nbtctl phones          # the paired phones
NBT_AGENT_SOCKET=/tmp/hu/agent.sock /tmp/nbt/nbtctl forget --all    # end every pairing
```

**How a link ends.** On a car, a link whose node the agent had closed while the phone app went on holding the
link, and sending `ping` into it, was followed by links on which nothing arrived: the car opened each new link,
the agent opened its node, and no byte of the phone's came until the head unit was restarted. That was seen each
time the app held such a link for 30 seconds or more (after `nbtctl forget --all`, after the agent ended for its
own update, and after the agent was stopped), and not when the phone followed the agent's close with a new link
within a second. What in the car's Bluetooth does not get over it is not known. The agent therefore does three
things:

- it tells the phone before it ends a link (`link.closing`, above), so that the phone app ends the link itself;
- when its own end of a link has closed, for whatever reason, it keeps the link's node open and goes on reading
  it, dropping what comes, until the phone has ended the link. It waits 5 seconds for that (`serial.lingerMs` in
  `spec/protocol/constants.txt`) and then closes the node under the phone. The link's last line in the log says
  which it was, and how much the phone still sent:

  ```
  bluetooth: /dev/btspp0 closed, closed by the agent, and the phone ended the link 180 ms later: 5210 B from the phone, 3984 B to it
  bluetooth: /dev/btspp0 closed, closed by the agent, and the link was still up 5000 ms later: its node is closed under it: 5210 B from the phone, 3984 B to it, and 36 B that the phone sent after the agent's close were dropped
  ```

- an agent that stops, or ends for a newer version, waits for that before it takes its service and its gate away
  and ends. An update of the agent is held up by 5 seconds at most. `kill -9` leaves no time for any of it.

A phone app that does not know the event still holds its link for up to 30 seconds, which is longer than the
agent waits. A link that the agent lets go as another program's (below) is not kept this way: its node is closed
after the decide step, as before. Status: passes `make agent-test`, with pseudo-terminals for nodes; whether it
is what the car's Bluetooth needs has not been tried on a car.

**Serial services of other programs.** The car's Bluetooth stack keeps a registration until someone removes it or
the stack restarts. A program that registers a serial service of its own and ends without removing it leaves it
behind: phones go on opening links to a service that nothing answers, and the car has one serial link for a phone,
so the phone's app does not reach the agent. The stack does not say what is registered, so the agent has to be told
which services there are and whose:

- a package says so in its manifest, with the name the registration is filed under and the file name of the
  program that makes it. Two installed packages cannot declare the same name:

  ```json
  "install": { "serialServices": [ { "name": "TestRadio", "program": "radiod" } ] }
  ```

- for a program that no package installed, a line in the file `serial-services` in the agent's state folder
  (`/var/opt/nbtapp`, or `state/` under `--root`): the name, a space, the program's file name. A line that starts
  with `#` is a comment.

With Bluetooth serial on, the agent removes the registration of every such service whose program is not running:
when its own service has been registered; during an install step once the hook that stops the package's
programs has had its turn (`preInstall` of an install or an upgrade, `preRemove` of a removal); and when it has
let a phone's link go, at most once in 30 seconds. It looks for the program among the head unit's processes by
the file name of the executable, so a program that a shell runs from a script is not found. A service whose
program runs is left alone, and so is every service when the processes cannot be read. Each removal, and what the
car's Bluetooth service answered, is in the log. Status: passes `make agent-test`, where the processes and the
Bluetooth service are the tests' own; not run on a car.

**Other programs' links.** The car gives every link a phone opens the same node and names the same service for
it, so a program that takes serial links of its own meets the agent's links, and the agent meets its links. Two
things keep them apart, both in the agent:

- The agent opens a link's node and reads what the phone says first. A link that opens with the protocol's magic
  bytes is the agent's. Any other is let go, at once if something else came first and after 500 ms if nothing
  came: the agent closes the node, reads no more of it and has written nothing into it, so the program the link
  belongs to finds it as it would have. What the phone had sent by then is lost to that program, so this suits a
  program whose links start with the car speaking, or a phone that sends again.
- From a phone's request for a link until that link has been let go, and for as long as a link of the agent's is
  up, the gate also refuses the opens of the programs that are declared for a serial service (above), so that none
  of them takes a link's first bytes or writes into the agent's link. Their looks at the node (`stat`, `access`)
  are passed on. A program that is not declared is not held off.

The log has, for every link, how long after the open its first message came, whether it was the agent's or was
let go and why, whether the node was still there after the close and when it went, and each time the gate starts
and stops refusing those programs. Not seen on a car: whether a link outlives the close of its node, and how long
after the open a phone's first message comes. Two options of `nbtagent` are for finding that out without a new
build, and go with Bluetooth serial only:

| Option | What it changes |
|---|---|
| `--decide <ms>` | How long the magic bytes are waited for, 1 to 10000, in place of the protocol's 500 ms. A phone app goes by the protocol's time, so this is for measuring, not for a car that is in use |
| `--decide-hold` | A link that is let go keeps its node open, unread, until the link ends, in place of having it closed. For a car on which a link does not outlive the close of its node: there the other program would lose every link the agent lets go. The agent reads and writes nothing on a node it keeps, and the gate's hold is off by then, so the other program can open the node beside it |

When Bluetooth serial has started, four lines in the log say what the agent goes by: the time and what becomes
of a link that is let go, what becomes of a link the agent ends, what it does about a registration that is not
taken or is lost, and the programs the gate holds off as they are declared at that moment (the list is taken anew
each time the hold goes on):

```
bluetooth: a link is the agent's if it opens with the magic bytes within 500 ms of the open of its node. Any other is let go: its node is closed for the program it belongs to
bluetooth: a link the agent ends keeps its node, read and with what comes dropped, until the phone ends the link, for 5000 ms at most
bluetooth: a serial service that the car's Bluetooth service does not take, or that is not registered for 20 s, is given up: the gate is taken away, and both are set up again 30 s later, for as long as it takes
bluetooth: while a link is being decided on or is the agent's, the gate also refuses opens by radiod
```

Status: passes `make agent-test`, which covers the options' value and these lines but not `nbtagent` taking them,
since only the head unit's build has them; not run on a car.

It ignores SIGHUP, so it goes on when the SSH session ends; `kill` with its process id stops it. Its log is its
standard error and nothing else, so the file above grows in `/tmp`, which is memory, for as long as the agent runs.
After a power down the programs are gone from `/tmp` and the agent has to be copied and started again.

### Kept on the head unit: the install script, the starter and a version's launcher

`hu_install.sh` copies the agent to `/var/opt/nbtapp/agent`, which a power down leaves alone: the two programs
and two scripts into a folder of their version's own, and the starter beside that folder.

- **The starter**, `nbtstart.sh`, is in the agent's folder as `launcher`. It is what a person or the iDrive
  plugin starts. It decides which version is started, and belongs to no version.
- **A version's launcher**, `nbtlaunch.sh`, is in the version's folder beside `nbtagent`. The starter runs it for
  the version it has chosen, and it starts that version's agent, and starts it again when it fails.

A new version of the agent therefore comes with its own launcher, which is tried with it: a launcher that does
not run on a head unit fails its version's try, and the version before is back, with the launcher it had. The
scripts are tested on the bench under Debian's `sh` (`make agent-test`); none has run under the head unit's `ksh`.

```sh
ksh /mnt/umassXXXXX/agent/hu_install.sh              # says what it did, and how to start the starter
/var/opt/nbtapp/agent/launcher --root /tmp/hu &      # the starter's arguments are nbtagent's
cat /tmp/nbtapp/log/agent.log                        # the log of this start; a power down empties /tmp
slay nbtagent                                        # SIGTERM to the agent: it ends, and its launcher and the starter with it
```

With no arguments the starter has the agent started on the head unit itself. Nothing starts the starter when the
head unit starts, until the iDrive plugin is asked to: "Starting with the head unit", below. Without a console, the
plugin's **Start the agent** screen starts it once: "Starting from the iDrive menu", below.

The agent's own package, `org.nbtapp.agent-<version>.nbtpkg`, holds the same first install. It is a zip, and its
`payload` folder holds `nbtagent`, `nbtctl`, `nbtlaunch.sh`, `nbtstart.sh`, `version` and `hu_install.sh`, which
is every file the install script asks for beside itself. Unpacked onto a USB stick, it is installed from there:

```sh
ksh /mnt/umassXXXXX/payload/hu_install.sh
```

The archive's `manifest.json` and `manifest.sig` are unpacked beside that folder and the script does not read
them: nothing on the head unit checks a first install's signature. The bench runs this with the package unpacked
into a folder (`tests/e2e/run_launcher.py`); it has not run on a car.

| In `/var/opt/nbtapp/agent` | Holds | Written by |
|---|---|---|
| `<version>/nbtagent`, `<version>/nbtctl` | One version's programs. A version's folder is never written over | `hu_install.sh`, or the agent from its own package |
| `<version>/nbtlaunch.sh` | That version's launcher | The same |
| `<version>/nbtstart.sh` | The starter that version came with | The same |
| `launcher` | The starter in place: a copy of one version's `nbtstart.sh` | `hu_install.sh`, or a version of the agent once it is confirmed, when the one it came with is another |
| `current` | One line: the version the starter has started | `hu_install.sh`, or a new version of the agent once a phone has reached it |
| `trial`, `trial.started` | A version that is to be tried once, and the same file once the try has begun | The agent that has placed a newer version of itself writes `trial`; the starter renames it; the agent that finishes or undoes the update removes it |
| `log/trial.log` | What the last try said that failed while the head unit was on | The starter, once, when a try fails |
| `autostart` | That it is there has the iDrive plugin start the starter when the HMI starts | The agent, when a phone or `nbtctl` asks (`autostart.set`); or by hand |
| `autostart.boots` | A line for each such start that no agent has outlasted by five minutes | The plugin, before it starts the starter; the agent removes it |

The install script writes over nothing and removes nothing. To put a version there that is there already (a new
build with the same version), stop the agent, remove that version's folder by hand, and run the script again; it
prints the command. It refuses while `trial` or `trial.started` is there.

Before each start the starter looks at the files again. With `trial` there, it renames it to `trial.started` and
runs the launcher of the version it names; with `trial.started` there, a try began and was not confirmed, and it
runs the launcher of the version in `current`; with neither, that of the version in `current`. A file that names
no version that is there, with its program and its launcher (a name starts with a digit and holds letters,
digits, `.`, `+` and `-`), is never followed.

What a version's launcher does when the agent ends depends on the agent's exit status (`agent/exit_status.hpp`),
and what the starter does next on the status the launcher ends with:

| Agent's exit status | Meaning | The version's launcher | The starter |
|---|---|---|---|
| 0 | The agent was stopped | Ends with 0 | Ends |
| 2 | It was started with arguments it does not take | Ends with 2 | Ends with 2 |
| 3 | A new start would change nothing: another agent has the socket, or this build does not know the head unit | Ends with 3 | Ends with 3 |
| 4 | The agent has placed a newer version of itself and asks to be started again | Ends with 4 | Looks at the files again at once, which starts that version |
| Any other, or a signal | A failure | Starts the agent again after 2 s; after five failures in a row it ends with 1 | Ends with 1 |

A version on trial gets one start. Its launcher ends with the agent's status whatever that is, and for the
starter any status but 0 and 4 is a try that failed, a launcher that did not run at all included: it copies the
start's log to `log/trial.log` and looks at the files again, which starts the version in `current`.

To stop the agent, stop the agent: its launcher and the starter follow. A starter or a launcher that is killed
leaves the agent running with nothing to start it again.

There is one starter to a head unit. A starter keeps its process id in `/tmp/nbtapp/log/launcher.pid` for as
long as it runs. A second one finds that file and the process it names running, says so, and ends with status 3
before it has looked at `trial`. A file whose process is gone was left by a starter that was killed, and the next
starter takes it over; a power down empties the RAM disk and the file with it. Only the process id is compared.

`NBT_LAUNCH_DELAY=<seconds>` (999 at most) in the starter's environment has the first launcher wait that long
before its first start, while the starter holds `launcher.pid`.

The starter is the one script that is tried with no version, because it is what gives a version its try. So it
changes only when a version is decided: an agent that is the version `current` names, with no update under way,
compares `launcher` with the `nbtstart.sh` in its own folder at each start and once it is confirmed, and puts
its own in place when the two differ and the shell can read it (`sh -n`). The file is written whole and renamed
into place. A starter that is running goes on as it was read, so the new one is what the next start of the head
unit runs. A start that finds the two the same writes nothing.

The starter has a revision of its own, `STARTER_REVISION` in `nbtstart.sh`, which is raised when that file
changes and by nothing else, so two versions of the agent that come with the same starter replace nothing. The
log of each start names it: `launcher: starter revision 2`, after what the starter read in the files where it
read something. The starter that agent 1.1.0 came with has no number and says nothing of it, so a log without
that line is its log.

#### Starting with the head unit

Nothing of the head unit's own starts a program from `/var/opt`. The HMI loads the iDrive plugin each time it
starts, so the plugin starts the launcher, when it is asked to (`src/agent_start.hpp`). What asks is the file
`autostart` in the agent's folder, and the agent makes and removes it when a person tells it to: from the phone
app, or at the console.

```sh
nbtctl autostart          # autostart on|off, whether it can be switched on here, and the starts on record
nbtctl autostart on       # the agent is started with the head unit from now on
nbtctl autostart off
```

These are the messages `autostart.get` and `autostart.set`, which an agent has that lists the capability
`autostart` in its `hello` (`spec/protocol/constants.txt`). What they answer:

| Member | |
|---|---|
| `on` | The file is there |
| `available` | Something would start the agent if it were switched on: a starter is in the agent's folder, and the iDrive plugin is installed as its package, `org.nbtapp.plugin`, in a version that starts the agent (1.0.0 or later) |
| `reason` | With `available` false, why: `no-launcher`, `plugin-missing` or `plugin-too-old` |
| `needs` | With the last two: the plugin's package id and the versions that would do |
| `starts` | The starts with the head unit on record that no agent outlasted by five minutes (below) |

- Switching on is refused, with the `reason` as the error's code, where it is not available. Switching off is
  always taken, so a file that was made by hand can be removed from the phone.
- Switching on forgets the starts on record: they are from before, and three of them would keep the plugin
  from starting anything.
- A phone that is paired and `nbtctl` may switch it. The iDrive plugin may ask how things stand, and is refused
  the switch (`forbidden`): it does the starting, and is not who decides it.
- An agent on trial refuses the switch with `busy`, because it writes nothing into its folder until it is
  confirmed.

By hand it is the file alone, which the agent does not hold against the plugin that is installed:

```sh
echo on > /var/opt/nbtapp/agent/autostart
rm /var/opt/nbtapp/agent/autostart
```

Each time the HMI starts, the plugin then starts `/bin/ksh /var/opt/nbtapp/agent/launcher` as a process of its
own, with `NBT_LAUNCH_DELAY=15` and the `PATH` and library path a shell on a head unit has, and writes one line
to the log (`sloginfo | grep NbtApp`, the lines that begin `agent:`). It starts nothing when:

- `autostart` is not there, or `/var/opt/nbtapp/disable` is;
- there is no launcher in the agent's folder;
- a launcher is running: the HMI starts more than once while a head unit is on (the agent restarts it after it
  has installed a plugin), and the launcher of the first time is left as it is;
- three starts with the head unit are on record that no agent outlasted by five minutes.

That record is `autostart.boots`. The agent puts a gate in front of the car's Bluetooth serial node; one that
brought a head unit down would do so again at every start, with nobody at a console to stop it. So the plugin
writes each start down before it makes it, and the agent removes the file once it has run for five minutes or has
been stopped. Three starts on record, and the plugin starts nothing and says `NOT started` in the log. A start
counts for 15 minutes; with the head unit's clock not set, which is how many start, it counts until the file is
removed. To have the agent started again: `rm /var/opt/nbtapp/agent/autostart.boots`, or start the launcher by
hand, at the console or from the iDrive menu (below), and leave the agent running for five minutes.

Status: passes `make agent-test`, where the plugin's code starts `nbtstart.sh` on a folder
(`tests/agent/test_agent_start.cpp`); the plugin has not run on a car. What a car has to show, with `autostart`
there:

```sh
pidin arg | grep nbt                          # after a start of the head unit: the launcher, and 15 s later nbtagent
sloginfo | grep NbtApp                        # agent: started the launcher (process ...)
cat /tmp/nbtapp/log/agent.log                 # when the serial service was registered, and whether at the first go
OnOffDSICommander appreset                    # restarts the HMI
pidin arg | grep nbt                          # the same two processes as before
sloginfo | grep NbtApp                        # agent: a launcher is running (process ...): left as it is
pidin -p nbtagent                             # the priority it runs at, which it has from the HMI's thread
```

Not known until then: whether a process the plugin starts goes on when the HMI is restarted, which the agent does
in the middle of a job; which priority and scheduling partition the agent gets that way; and whether 15 seconds is
long enough for the car's Bluetooth service to be there (if not, the agent sets its serial service up again 30
seconds later, as above).

#### Starting from the iDrive menu

A head unit with no console starts its first agent from the plugin's menu: **NBTApp**, then **Start the agent**.
The screen asks the agent's socket whether an agent answers (a `ping`, after the plugin's `hello`) and says so.
With none, its second row, **Start it now**, starts the starter as the plugin does with the head unit, but with no
wait (`NBT_LAUNCH_DELAY=0`), and the screen asks again once a second until an agent answers, for 30 seconds at
most. Once it says **Started. A phone can connect now**, the phone app can pair (**Pair a phone**) and switch
`autostart` on, and from the next start of the head unit the agent is started with it.

A start by hand (`startByHand` in `src/agent_start.hpp`) asks for less than a start with the head unit:

- not for `autostart`: a person asks, there and then;
- not for the starts on record in `autostart.boots`, which it neither reads nor writes. That record keeps a head
  unit from starting a bad agent at every start with nobody there; a start by hand is one start with somebody
  there. An agent started this way that runs for five minutes removes the record, as any agent does, so this is
  how the record is cleared without a console.

It starts nothing where there is no launcher in the agent's folder (the screen says **The agent is not
installed**), or where a launcher is running (the screen waits for its agent). With `/var/opt/nbtapp/disable`
there the plugin has no menu at all. Each press that starts something is one line in the log: `agent: started the
launcher by hand (process ...)`.

Status: passes `make agent-test`, where the start by hand starts `nbtstart.sh` on a folder
(`tests/agent/test_agent_start.cpp`) and the screen runs against an agent (`tests/agent/test_plugin.cpp`); the
plugin has not run on a car. What a car has to show is what is listed above for a start with the head unit, with
a press in place of the start, and whether an agent started this way goes on when the HMI is restarted.

#### The agent updates itself

The agent's own package is `org.nbtapp.agent` (`tools/agent_pkg.py` makes it). It places `nbtagent`, `nbtctl`,
the version's launcher `nbtlaunch.sh` and the starter the version comes with, `nbtstart.sh`, all four in the
version's own folder, and is planned and fetched like any package. It also holds `hu_install.sh` and `version`,
which a first install needs and an update places nowhere. So an update brings everything a version runs with,
and nothing of it is in use before the version is: the launcher is tried with the agent, and the starter in
place changes once the version is confirmed. The step that installs it differs (`agent/engine.hpp`, the plan
document's "Agent updates"):

1. The new version goes into a folder of its own, `/var/opt/nbtapp/agent/<version>`. The version that runs is
   not touched. The new `nbtagent` is asked for its version there and then, and the shell reads the new launcher
   (`sh -n`); a program that does not start or says another version than its package, or a launcher the shell
   cannot read, fails the step and never gets a try.
2. The step is not committed. The agent writes `trial`, reports the job as `restarting`, and ends with status 4.
3. The starter renames `trial` to `trial.started` and runs the new version's launcher, which starts the new
   version. That agent finds itself on trial. It answers its links, and leaves the job's record, the journal and
   the install log as the old version wrote them.
4. A phone connects again and asks for the job with `job.status`. That is the confirmation: the new version
   writes its name to `current`, commits the step, removes `trial.started`, and the job has succeeded. It then
   puts its starter in place of `launcher` if that is another one. The folder of the version before it stays,
   and those of older versions are removed.

If the new version falls over, its launcher does not run, or the head unit is turned off before a phone has
asked, the starter finds `trial.started` and runs the launcher of the version in `current`, which is still the
old one. That agent undoes the step, removes the new version's folder and `trial.started`, and reports the job
as failed with `not-confirmed`. The starter in place is as it was. The update can be run again.

What an agent accepts of its own package:

- Only where a starter starts it: `current` names the version that runs, and that version's program and its
  launcher are in the folder of that name. Anywhere else the plan has the problem `no-launcher`, because nothing
  would start the new version. With `--root DIR` the folder is `DIR/agent`.
- Alone in its job (`conflict` otherwise): what came after it would be run by a version that is not confirmed.
- Higher than the version that runs, whether or not that version has an installed record. `hu_install.sh` leaves
  none, so the first update finds none.
- Signed by a key that is built in, as for every `org.nbtapp.` id. There is one such key
  (`agent/embedded_keys.hpp`).
- Files in `${app}` and `${data}` only, with `${app}/nbtagent` as a program and `${app}/nbtlaunch.sh` beside it.
  It cannot be removed.
- From the agent it names in `requires.agent`. The package of this repo names 1.1.0, the first version whose
  folder holds a launcher: the versions before it were for trials, and a head unit that has one is installed
  anew with `hu_install.sh`.

A `job.status` from the iDrive plugin does not confirm a version: the plugin would reach an agent that no phone
can. `nbtctl status` does, as a person at the console.

The launcher and the starter as they are from 1.1.0 have not run on a car. The bench runs an update with two
builds of `nbtagent` that differ in the version they say they are, with a starter in place that is another one
than the new version's, and runs a new version whose launcher does not run (`tests/e2e/run_launcher.py`); it
ends the agent at every point of the three parts (`tests/agent/test_engine.cpp`). The catalogue lists
`org.nbtapp.agent`, and the phone app follows a job through `restarting`.

#### Releasing a version of the agent

A version of the agent reaches a car in one of two ways: for the first install, `release/agent/` and
`hu_install.sh`; for every version after it, the package `org.nbtapp.agent`. `tools/agent_pkg.py` makes the
archive from the programs in `release/agent/` and `agent/package/manifest.json.in`, and the same programs give the
same bytes. When the plugin is released with it, the order is under
[Releasing the agent and the plugin](#releasing-the-agent-and-the-plugin).

1. Set the version in `agent/version.hpp` and commit. A version that is published is never changed, so what is
   signed has to be a commit.
2. `./build.sh agent` on the clean tree. It leaves the package, not signed, in `release/pkg/`.
3. `make agent-release KEY=FILE`, with the private half of the package key. It makes the package again, runs
   `make agent-check`, and writes the signed archive to `release/pkg/signed/`, with its SHA-256. It refuses:
   - programs built from a tree with changes that are not committed, or from another commit than the one that is
     checked out;
   - a key whose public half is not in `agent/embedded_keys.hpp`, because no agent would take the package;
   - a signed archive of that version that is there already with other bytes.
4. In the catalogue repo, `tools/nbtcat.py add-version` with that archive and no URL, and then its
   `tools/publish.py`. The archive goes into that repo and the catalogue serves it from its own address,
   `https://catalogue.nbtapps.com`, so this repo makes no release of its own for it.

The private half of the package key is in neither repo, and no build or test here reads it:
`tests/test_agent_pkg.py` signs with keys it makes for the run.

#### What is written to flash

`/var/opt/nbtapp` is on flash, and a head unit starts many times a day. A start that has nothing to finish writes
nothing there:

- **The starter** reads `current` and runs that version's launcher, which starts the agent. Their log, and the
  agent's, is `/tmp/nbtapp/log/agent.log` on the RAM disk, with `agent.log.1` and `agent.log.2` for the two
  starts before. A power down empties it. That is so for a version on trial too. A version's launcher writes
  nothing to flash.
- **The starter, for a version on trial**, renames `trial` to `trial.started`. If that version's launcher then
  ends with any status but 0 and 4, the starter copies that start's log to `log/trial.log` in the agent's folder,
  once, over the copy of the try before. A try that works writes no log to flash. A start that finds
  `trial.started` left from a try writes nothing: its log says whether there is a copy.
- **A start with the head unit** (`autostart`, above) writes one line to `autostart.boots` before the launcher is
  started, and the agent removes the file five minutes later, or when it is stopped. A start by hand writes
  neither, and a head unit with no `autostart` file is written to by none of it.
- **The agent** reads its state and staging at a start and writes neither, unless an install was cut off and has
  to be finished or undone, or staging holds files from before a power down. It reads the starter in place and
  the one in its own folder, and writes the starter only when they differ, which is once after an update that
  brought another one. While it runs it writes when a job
  runs: what it fetches, what it installs or removes, and the job's record. A phone that connects, says hello and
  asks what is installed causes no write.
- **A version of the agent on trial** writes none of its state until a phone has confirmed it: the job's
  record, the journal and the install log stay as the version before left them, for that version to read if it
  comes back. The confirmation then writes `current`, the package's record and the job's.
- **The log's size**: a version's launcher names the log's file to the agent (`NBTAGENT_LOG`), and the agent keeps it
  short. A file that has passed 512 KB becomes `agent.log.1`, over the one that was there, and the log goes on in
  a new file, so the three files hold 1.5 MB at most.

`make agent-test` holds every file and folder against what it was before such a start: name, size, mode, inode and
the times of the last change. It does not look at a file's time of last access, and whether the head unit's file
system writes that down when a file is read is not known.

What an agent says is on the RAM disk and is gone after a power down; the copy above is the one exception. A
try that a power down ends leaves no log, and that is how a try ends whose agent no phone could reach. To find
out why, try the update again and read the log before the head unit is turned off.

## Releasing the agent and the plugin

The agent and the iDrive plugin are two packages with a version each (`agent/version.hpp`, `src/version.hpp`),
and a head unit gets them one after the other: an update of the agent is alone in its job, and the plugin's
install ends with a restart of the HMI. So every head unit that takes both runs the new agent with the plugin
from before for a while, and many will for good. A feature that takes both, as the start with the head unit
does, is released by these rules.

**1. An agent runs with every plugin that has been released, and with none.** No version of the agent needs a
plugin to be installed, to start, or to do what the version before it did. What an agent keeps as it was for the
plugins that are out:

- the messages a plugin sends on the local socket, which are `hello` with the role `plugin`, `pairing.open`,
  `pairing.close` and `phones.forget`, and what they answer;
- the files a plugin goes by when it starts the agent: `launcher`, `autostart` and `autostart.boots` in
  `/var/opt/nbtapp/agent`, `/tmp/nbtapp/log/launcher.pid`, and `/var/opt/nbtapp/disable`. A plugin starts
  `launcher` with `/bin/ksh` and `NBT_LAUNCH_DELAY`, so a starter of any version takes both.

**2. The requirement is the plugin's.** A plugin that uses anything an older agent lacks names the first agent
that has it in `requires.agent` (`plugin/package/manifest.json.in`). An agent below that refuses the package with
`agent-too-old` when the job is planned, before anything is fetched or written. The phone app does not offer
such a version for install: it offers the agent's update, names the plugin as something the update brings, and
lists the plugin under **Now available** once the new agent is confirmed. The agent's own package names no
plugin and never will: the plugin could not be installed first.

**3. What an agent cannot do with the plugin that is there, it says.** A feature of the agent that needs a
plugin, or a newer one, is switched off on a head unit without it, and the agent answers what is missing: in
`autostart.get`, `available` false, a `reason` (`plugin-missing`, `plugin-too-old`), and `needs` with the
package's id and the versions that would do. The range of versions is in the agent, held against the plugin's
installed record (`kAutostartPluginRange` in `agent/agent_home.hpp`). The phone app makes a button to the
package's page of that answer. A new feature of this kind gets a range of its own, an answer of this shape, and
a capability in `hello`, so that an app can tell an agent without the feature from one without the plugin.

**4. The agent is released first.** For a feature that takes both:

1. The agent: raise `agent/version.hpp`, add the range of rule 3, `./build.sh agent`,
   `make agent-release KEY=FILE`, and record the archive in the catalogue repo.
2. The plugin: raise `src/version.hpp`, and `requires.agent` where rule 2 asks for it, `./build.sh`,
   `make plugin-release KEY=FILE`, and record that archive.
3. Publish the catalogue with both in it, or with the agent alone and the plugin later. Never the plugin alone:
   the catalogue's tool does not hold a version's `requires.agent` against the agent versions it lists, and a
   plugin that names an agent nobody can get is a version no head unit installs.

Neither number is raised because the other was. A plugin's version says nothing of an agent's, and
`requires.agent` changes only when the plugin begins to use something newer.

The agent holds a package to `requires.agent` when it installs it. A plugin that is copied on by hand
("By hand", above) is held to nothing, and neither is an agent that `hu_install.sh` puts in place over a plugin.
Rule 1 covers the second. For the first, an agent answers a message it does not know with `unsupported`.

On a head unit the two updates are two jobs from the phone: the agent's, which ends when a phone has confirmed
the new version, and then the plugin's, which restarts the HMI. No pair of versions has gone through this on a
car.

## License

PolyForm Strict License 1.0.0, as the whole repo: see [LICENSE.md](../LICENSE.md) at its root.

`agent/vendor/tweetnacl/` is third-party code in the public domain; see the README in that folder.

Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)

## Disclaimer

This loads code into the head unit's HMI process. A fault there can stop iDrive from starting. The code comes as is;
you are responsible for what you do to your own car.

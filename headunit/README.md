# NBTApp

A native iDrive app for BMW NBT Evo head units (iDrive 5/6, QNX Neutrino 6.5.0 SP1, ARMv7LE). For now it is
small: an **NBTApp** entry in Media/Radio that opens a menu with six rows, **Start the agent**, **Stop the agent**,
**Pair a phone**, **Forget all phones**, **Reset Bluetooth link** and **About**. The first starts the install agent
on a head unit with no console, and the second stops it until the head unit has powered down (see "Stopping from
the iDrive menu"); the next two are the car's side of pairing a phone app with it (see
[Packages and the agent](#packages-and-the-agent)), and the fifth has the car end the phone's Bluetooth serial
link, for a phone app that connects and gets nothing (see "How a link ends").

It is an iDrive **PluginApp**, built on the **HU Framework**: the head unit's own framework for add-on apps such as
`SecurityMenu` and `ActiveSeating` (`libAppFramework.A.so`, `libCoreFramework.A.so`). At boot, HmiMain3D loads every
`Plugins/<Name>/<Name>.manifest` under `/opt/hmi/ID5/data/ro`, and those under `/var/opt/hmi/data/ID5`, on the
writable partition. The entry button and its placement come from the XML, so iDrive creates the menu row itself.
Nothing in HmiMain3D is patched.

## Layout

| Path | Purpose |
|---|---|
| `src/nbt_app.cpp` | The app (`pluginAppInfo` entry point). Fills the About text and drives the five screens that talk to the agent; the menu and its steps are XML |
| `src/agent_start.hpp` | The start of the agent's launcher when the HMI starts, on a head unit that is asked to start the agent by itself, and the record that keeps an agent which brings the head unit down from being started for ever, and the mark of a stop that keeps it from starting one that was stopped; and its start by hand from the Start the agent screen |
| `src/phone_screens.hpp`, `src/agent_link.hpp` | What the Start the agent, Stop the agent, Pair a phone, Forget all phones and Reset Bluetooth link screens ask of the agent, and when; and their link to it, the agent's local socket |
| `src/hu_framework.hpp` | Declarations of the HU Framework (it ships no headers) |
| `src/version.hpp` | The plugin's version, which its package is released as |
| `plugin/NbtApp.manifest` | Tells iDrive what to load |
| `plugin/NbtApp.xml` | The UI: entry in Media/Radio after the Bluetooth audio row, menu (`hmiState` 10), Start the agent (`hmiState` 16), Stop the agent (`hmiState` 18), Pair a phone (`hmiState` 14), Forget all phones (`hmiState` 15), Reset Bluetooth link (`hmiState` 17), About page (`hmiState` 12), preview (`hmiState` 11) |
| `assets/` | Images: 63x54 row icon, 549x484 preview |
| `stubs/*.syms` | Symbol names of the two HU Framework libraries. The build makes empty link stubs from them, so no head-unit binary is needed |
| `tools/make_db.py` | The texts and image ids; packs the text and image DBs |
| `plugin/package/manifest.json.in` | The manifest of the plugin's own package, `org.nbtapp.plugin` |
| `tools/plugin_pkg.py` | Builds that package of `release/NbtApp/` (`make plugin-pkg`) and signs it for a release (`make plugin-release`) |
| `tools/check.py` | `make check` |
| `tools/make_icons.py`, `tools/update_stubs.sh` | Redraw the images; regenerate `stubs/*.syms` from a firmware dump |
| `agent/` | The install agent: the package reader (zip profile, signature, manifest, hashes), the install engine (install, upgrade, remove, rollback, recovery), and the protocol a phone speaks to it (frames, messages, plans, jobs), on SHA-256, a strict JSON parser, inflate, and Ed25519 verification from `agent/vendor/tweetnacl`. Two programs: `nbtagent`, which runs on a folder that stands in for the head unit and, in the build for the head unit, also on the head unit itself, and `nbtctl`, a command-line controller. Also here is the Bluetooth serial link, which only the head unit's build of `nbtagent` offers: `dsi_client` (the head unit's service-to-service IPC), `spp_service` (the registration of a serial service of our own with the Bluetooth stack, when it may go, and the stand-ins ahead of it), `spp_gate` (the gate in front of `/dev/btspp0`), `spp_nodes` (which serial node holds a link of ours), `node_reader` (the one reader of an open serial node, which keeps a read out on it), `serial_link` (the listener, and a link on a serial node), `programs` (which programs are running), `serial_services` (the serial services other programs register, the agent's stand-in for each, and the replacement of what one whose program is gone left), `serial_report` (the start-up report: what the head unit says of its Bluetooth when the agent starts), `presence` (which of the car's phones are there, from its device list), `dial_rules` and `dial_config` (when the car dials a phone of the agent's, and what each answer comes to), `serial_dial` (what the agent and the listener tell each other of the dial and of `serial.status`) and `problem_log` (the problem file). `agent_home` is the agent's own folder on the head unit, a folder to each version of it, which the agent updates itself through. `checks` holds the compatibility checks: what was put onto a head unit by hand, outside this system, and its fix: a plugin moved aside whole, the rest removed a path at a time (see [Checks](#checks)). `settings` and `settings_file` are a package's settings: the rules a value is held to, where the file is, and its text edited a key at a time. `channels` is the channels that are open: a controller's messages to a package's program and back, and the programs the agent starts for them. Four scripts for the head unit's shell: `hu_probe.sh` reads off a head unit what the agent has to know about it, `hu_install.sh` puts the agent onto one, `nbtstart.sh` is the starter that decides there which version of the agent is started, and `nbtlaunch.sh` is a version's own launcher, which starts that version's `nbtagent` and starts it again when it fails |
| `spec/` | Definitions shared with the Android app: `manifest.schema.json`; `json-profile.vectors`, the cases every JSON parser of the project must agree on; and `protocol/`, the frames every codec must build and refuse alike, one whole conversation with the agent, frame by frame, and `constants.txt`, the names and numbers both sides must hold alike |
| `tools/nbtpkg.py` | Builds, signs and verifies `.nbtpkg` packages |
| `tools/check_agent.py` | `make agent-check`: `release/agent/` is the head unit's kind of program, holds nothing of the test bench, and its `nbtagent` holds the official package key; and the agent's own package in `release/pkg/` is byte for byte what `tools/agent_pkg.py` makes of that folder |
| `agent/package/manifest.json.in`, `tools/agent_pkg.py` | The agent's own package, `org.nbtapp.agent`: its manifest, with a placeholder for the version, and the script that makes the archive of `release/agent/` (`make agent-pkg`) and signs it for a release (`make agent-release KEY=FILE`); `tests/test_agent_pkg.py` tests the script |
| `tests/` | Host tests of the Python tools: `test_nbtpkg.py` (`make test-pkg`), `test_agent_pkg.py` (`make test-agent-pkg`), `test_plugin_pkg.py` (`make test-plugin-pkg`) and `test_ssh_pkg.py` (`make test-ssh-pkg`) |
| `tests/agent/` | Tests of `agent/` (`make agent-test`), run in a Docker container (`tools/bench.py`), and `chan_echo.cpp` and `chan_feed.cpp`, the programs of the test package chan's channels; `tests/compare_readers.py` holds the agent's package reader and the tool's together, `tests/test_protocol_vectors.py` reads `spec/protocol/` with a codec of its own, `tests/e2e/run_agent.py` runs `nbtagent` and `nbtctl` as two processes, `tests/e2e/run_ssh.py` logs in through the SSH package's bench form with an OpenSSH client, and `tests/e2e/run_launcher.py` runs the starter, a version's launcher and the install script on folders |
| `tests/packages/`, `tests/keys/` | Package sources and test keys for `tests/make_packages.py`, which builds good and deliberately broken packages (`make testdata`); `tests/test_nbtpkg.py` tests the tool against them (`make test-pkg`) |
| `ssh/` | Third party: Dropbear's SSH server and OpenSSH's `sftp-server`, as git submodules pinned at release tags (`ssh/dropbear`, `ssh/openssh`), with our patch to Dropbear, its build options and the build scripts, for the head unit and for this machine; `tools/check_ssh.py` is `make ssh-check`. `ssh/README` says what each change is for and how the sources were checked against the signed releases. Ours: `ssh/relay/`, `sshrelay`, the program of the SSH package's channel, which starts a Dropbear server for each connection and carries its bytes (`ssh/README`, "The relay"), and its `version.hpp`, the package's version; its tests are `tests/ssh/` (`make ssh-test`). `ssh/package/manifest.json.in` is the manifest of the SSH package, `org.nbtapp.ssh`, and `tools/ssh_pkg.py` makes it (`make ssh-pkg`) and signs it for a release (`make ssh-release KEY=FILE`); see [The SSH package](#the-ssh-package) |
| `build/`, `release/` | Build products, not tracked. `release/NbtApp/` is the folder for the head unit; `release/agent/` holds the agent's programs for it, and `release/pkg/` the plugin's, the agent's own and the SSH package |

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

Dropbear and OpenSSH's `sftp-server` (`ssh/`, see `ssh/README`) are built from the submodules, which
`make ssh-src` fetches when they are not there (a plain `git clone` leaves them out), and the relay from `ssh/relay/`:

```sh
make ssh-src      # the submodules' pinned commits exported to build/ssh/src/, Dropbear's patched; the submodules are only read
make ssh          # -> build/ssh/qnx/: dropbearmulti and sftp-server for the head unit, in the SDP image (docker; no QNX licence)
make ssh-host     # -> build/ssh/host/: the same two for this machine
make ssh-relay-host  # -> build/ssh/host/sshrelay: the relay for this machine, with its test hooks
make ssh-test     # the relay's tests on this machine (after make ssh-host); the last line says what was skipped
make ssh-check    # the builds looked at; the head unit's are not run
QNX_LICENSE_DIR=/path/to/qnx/license ./build.sh ssh   # all of it: ssh-host, ssh-test, ssh, the relay for the head unit
                                                      # (make ssh-relay, qcc, in the image) -> build/ssh/qnx/sshrelay, ssh-check,
                                                      # and the SSH package -> release/pkg/ (make ssh-pkg, not signed)
```

## Packages and the agent

The agent installs signed `.nbtpkg` packages that a controller offers it. It reads and verifies a package, and
installs, upgrades and removes it with backups, rollback and recovery after a power cut; it plans a job from targets
and offers, pulls the archives it needs over the link, and reports as it goes. The plan document sets the package
format and the protocol.

- With `--root`, `nbtagent` keeps everything under that folder, which stands in for the head unit. The code for a
  real one is `agent/hu_platform.cpp`: it remounts the system partition for a write and puts it back, and
  restarts the HMI, by running commands from a configuration.
- The build for the head unit (`./build.sh agent`) uses that code when `nbtagent` is started with no `--root`: it
  then works on the head unit itself, with its state in `/var/opt/nbtapp` and its socket at `/tmp/nbtagent.sock`,
  where that build's `nbtctl` looks when it is told nothing else. The configuration for a car is
  `HeadUnitConfig::car()`: `${app}` is `/var/opt/nbtapp/apps/<id>`, `${plugins}` is
  `/opt/hmi/ID5/data/ro/Plugins` and `${var_plugins}` is `/var/opt/hmi/data/ID5/Plugins`, the system partition is
  `/fs/sda0`, remounted with `/bin/mount -u -o rw /fs/sda0` and put back with `-o ro`, and the HMI is restarted
  with `/opt/core/bin/OnOffDSICommander appreset`. The file says where each value comes from. After each remount
  the agent asks the file system whether it is read-only, so a remount that does nothing fails the install step
  before a file is written, or leaves the partition writable until the head unit is turned off and says so in
  the install log. No command for a reboot is known: the agent answers a request for one with `unsupported`, and
  the head unit has to be turned off and on by hand. On a machine that has no `/fs/sda0`, or no
  `/opt/hmi/ID5/data/ro` to hold the plugin folder, this mode ends without making anything; nothing is asked of
  `/var/opt/hmi`. The host's build has no such mode.
- A package places its files in four folders and at absolute paths (`install.files`, `install.links`): `${app}`
  and `${data}`, its own; `${plugins}`, the HMI's plugin folder on the system partition, which the agent remounts
  for the step; and `${var_plugins}`, the second folder the HMI loads plugins from, on the writable partition
  behind `/var`, where apps installed by hand are put and which needs no remount. The two plugin folders are
  alike in everything else. Each is one of the package's own places, so a package that places files there is not
  flagged `modifies-system`. A head unit that never had a plugin in one has no such folder: the first package
  that places a file there makes it, and takes it away again when that package goes and the folder is empty. The
  folder that holds it is never made: where it is not there, as for `${var_plugins}` on a head unit without
  `/var/opt/hmi/data/ID5`, whose HMI does not load plugins from there, a package that places a file or a link in
  it is refused with `stock-mismatch` when the job is planned, before anything is fetched. The agent writes over
  no file there that is in no package's record (`stock-mismatch`). A plugin's connector id and
  focus ids are checked against the apps in both folders, whichever it goes into. So is its name, the folder it
  has in the plugin folder: where another app's plugin of that name is in the other folder, or in any other folder
  the HMI loads plugins from, the package is refused with `hmi-id-taken`, as for an id in use, because two plugins
  of one name would be confusing. A folder of that name that holds no `.manifest` does not count, and neither do
  the package's own files, so an upgrade may move its plugin from one folder to the other. Hooks get the folder as
  `NBT_VAR_PLUGINS_DIR`. `${var_plugins}` is not a folder a `logs` entry may name. An agent that does not know
  `${var_plugins}` refuses a manifest that uses it, so a package that places anything there sets
  `requires.agent` to the first agent that knows it, 1.6.0: an older agent then refuses it with `agent-too-old`
  instead of as a bad manifest, and the phone app says that the package needs a newer agent. `nbtpkg.py` holds
  the package's author to that, as it does with a `logs` path, and is stricter there than the agent: it refuses
  (`bad-manifest`) a manifest that places a file or a link in `${var_plugins}` unless `requires.agent` has a
  comparator `>=`, `>` or `=` (or none) at 1.6.0 or above, the three numbers compared and a pre-release part not
  looked at. A phone app that does not know `${var_plugins}` and reads `install` strictly, as the apps from
  before `requires.app` do, refuses such a package as a bad manifest when it downloads it, whatever agent the head
  unit has. Such a package's record is one that an older agent cannot read (see "Once a version is confirmed"
  under [The agent updates itself](#the-agent-updates-itself)). On the bench, with `--root DIR`, the folder is
  `DIR/var-plugins`.
- A removal takes away the files and links the package placed, then its folders, each only if it is empty and
  the deepest first: the folders the package made, and every folder below a plugin folder that it placed a file
  or a link in, whoever made it, such as a plugin's folder that was there before the package was installed. A
  plugin folder itself (`${plugins}`, `${var_plugins}`) goes only if this package made it. `${app}` goes whole,
  and `${data}` goes whole or stays as the user chooses. The agent never removes a file the package did not
  place, so a folder that still holds one stays. The install log names such a folder (`left <folder>: it is not
  empty`) when the package made it, unless it is a plugin folder itself. A folder the package only placed files
  in may be another app's, and stays without a word; so does the plugin's folder of a package that was installed
  again over the folder an earlier removal left. An upgrade does the same, after its commit, with the folders
  that only the old version used, naming those the old version made, and an install that is undone with the
  folders it made. After a power cut the agent finishes an upgrade without naming any folder: its journal does
  not say which ones the old version made. A plugin in `${var_plugins}` can write into its own folder
  while it runs, and what it writes there keeps that folder on the head unit after the package is gone. So a
  plugin that writes files at run time keeps them in `${data}`, the package's data folder
  (`/var/opt/nbtapp/data/<id>` on a car), or removes them in its `preRemove` hook, which finds the folder in
  `NBT_VAR_PLUGINS_DIR`.
- A manifest's `requires` holds two version ranges. `requires.agent` is the agents that install the package: an
  agent outside it refuses the package with `agent-too-old` when the job is planned, and reads it before the rest
  of the manifest, so that a package that uses a later format of the manifest names the first agent that knows
  that format and an older agent does not call it broken. `requires.app` is the versions of the phone app that may
  install the package: the app checks it against its own version, and neither offers nor downloads a version that
  leaves it out. The agent takes it, checks that it is a range and does not act on it. An agent knows
  `requires.app` from 1.6.0 and an older one refuses a manifest that has it (`unsupported-manifest`), so a manifest
  that has it names 1.6.0 or later in `requires.agent`; `nbtpkg.py` refuses (`bad-manifest`) one that does not, by
  the rule it has for `${var_plugins}`. A phone app that knows `requires.app` reads the rest of `install` and
  `requires` leniently: a folder variable, a key, a `reboot` value, a hook name or a requirement that it does not
  know does not stop it downloading the package. It works out the risk flags from what it knows and says that the
  package uses features it does not know. The agent has the last word on all of it.
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
- The links are a local socket; in the head unit's build, Bluetooth serial (see below); and in the host build
  only, a TCP port on the loopback address (`--debug-tcp`), for the phone app's debug build on an emulator.
  TCP for a car, with the frame MAC it needs, is still to come.
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
  reference tab).
- On a Bluetooth serial link the agent gives 4,096 bytes as its largest frame, sends `ping` after 10 seconds with
  nothing heard, and ends the link after 30; a serial link with no `hello` 30 seconds after it opened is ended
  too. A serial link on which the protocol's magic bytes have not come 500 ms after the agent opened it, after any
  whole frames that an earlier link left unread, is not the agent's, and is let go. Those numbers, with the serial
  service's name and UUID, are in `spec/protocol/constants.txt`, which the phone app has to keep to as well.
- An agent that takes Bluetooth serial links has `serial.reset` and lists the capability `serial-reset` in
  `hello`; any other answers it with `unsupported`. It asks the car's Bluetooth service to end the phone's serial
  link, which closing the link's node does not do: with a serial node open, about 300 ms after its answer
  (`{"state":"sent"}`), and with none, at the open of the next serial link within 60 seconds
  (`{"state":"armed","forMs":60000}`). It is refused with `busy` while a job runs, and with `io-error` when the
  car's Bluetooth service cannot be asked. `nbtctl serial reset` asks for it, and so does the plugin's **Reset
  Bluetooth link** screen (see "How a link ends" below). A link that the car dialled (below) is ended as the agent
  ends a link, with `link.closing` and the reason `reset` after the answer, which is `sent`.
- The car can dial the phone, too: an agent whose Bluetooth serial listener dials lists the capability
  `serial-dial` in `hello`, and a phone app that listens on the agent's UUID is then reached on a link the car
  opens (see "The car dials the phone" below). Such a link opens with the phone's magic bytes and `hello`, as one
  the phone opened. Should a link of each kind come up, the first to finish `hello` stays and the other is ended
  with `link.closing` and the reason `other-link`. An agent whose car dials a phone when its app knocks (the
  default, see "The knock" below) also lists `serial-knock`; a phone app knocks only at an agent that lists it.
- `serial.status` says how Bluetooth serial stands, and `info.get` has the same in its member `serial`: whether
  the service is registered and how long ago it was last checked, the Bluetooth stack's version strings, how many
  phones are connected to the car and how many of them the car may dial, whether the dial is on, whether it waits
  for knocks (`knock`, true or false), where it is and
  what the last dial came to, which of the agent's serial links are up (`none`, `phone-opened`, `dialled` or
  `both`), whether a failed dial may have left the car's record of phone-opened links wrong (`recordDoubtful`),
  whether the re-points after failed dials have stopped (`repointStopped`, see "The re-point" below),
  for a phone's link whether a link that phone opens itself is left to another program now (`phoneOpenedLeft`,
  see "Other programs' links"; not there on the local socket), counts of the asks before the dials, of the dials, their answers, the re-points and the registration checks, of the links
  phones opened that the rule left to another program unread (`leftUnread`), of the links on which bytes the agent
  had read were put back for another program (`putBack`; both in "Other programs' links"), of the knocks
  (`knocksByMarker`, the ones seen, by the knock's magic bytes; `knocksIgnoredQuiet`, `knocksIgnoredBusy` and `knocksIgnoredNotDialable`, the ones ignored and why, as in "The knock"; `knockDials` and
  `knockDialsFailed`, the dials knocks brought and the ones of them that gave the agent no link), of the refusals
  whose look at the car's device list found the phone with no media audio (`refusalsNoAudio`, with the refusal's
  log line below), and how many
  problems were written to the problem file and how many only counted. The count `notListening` is of the dials
  answered 5; with `--dial-knock off` every dial follows an ask that listed the agent's service, so there it does not
  mean the app was not listening (the name is kept for the app's sake). Never a phone's address or name. An agent
  with no Bluetooth serial listener answers `serial.status` with `unsupported`, and its `info.get` has
  `"serial": {"listener": false}`. `serial.dial`, on the local socket only, has the car dial the phones connected
  to it now (`{"phones": N}`);
  `nbtctl serial status` and `nbtctl serial dial` send them. `agent/agent.hpp` has the members one by one.
- `agent.stop`, on the local socket only, stops the agent for a person who asked for that: `nbtctl stop` sends
  it, and so does the plugin's **Stop the agent** screen. It takes no params, is refused with `forbidden` on any
  other link, so no phone can stop the agent it talks to, and with `busy` while a job runs or a restart is on its
  way. An agent on trial may be stopped, and its try then ends unconfirmed. The answer is `{}`, and once it is
  written out (two seconds at most) the agent ends as it does for SIGTERM: the phones are told `link.closing` with
  the reason `stopping`, Bluetooth serial is taken away, and the exit status is 0, so its launcher and the starter
  end too. On its way out it leaves the empty file `agent.stopped` in its run folder on the RAM disk
  (`/tmp/nbtapp`, or `DIR/run` with `--root`), which keeps the iDrive plugin (1.3.0 and later) from starting it
  with the head unit until the head unit has powered down ("Stopping from the iDrive menu", below); the next agent
  that starts removes it, and says so. A signal leaves no such file.
- The phone made a link, and the phone ends it. When the agent ends a link of its own accord it says so first,
  with the event `link.closing`, and closes its end once that is written out; nothing follows the event. Its
  `reason` is `stopping` (the agent's process ends), `restarting` (it ends for a newer version of itself, which
  the launcher starts), `forgotten` (the phone's pairing was ended on the head unit), `replaced` (a newer link
  from the same phone took this one's place), `silent` (the phone answered neither the pings nor a
  `blob.fetch`), `reset` (`serial.reset` ended a link the car dialled) or `other-link` (a Bluetooth serial link of
  the other kind did `hello` first). A phone app closes its side as soon as the event arrives. The local socket's
  links get no such event, and neither does a link that ends for what the phone did wrong: the error in the answer
  says why. Why this matters on Bluetooth serial is under "How a link ends" below.
- A controller can pull a package's log files, so that a user can send them to whoever asked. A package names them
  in its manifest's `logs`: files or a folder with a wildcard, in `${app}`, `${data}`, `${tmp}` (the package's own
  folder on the RAM disk, `/tmp/nbtapp/apps/<id>` on a car, which the agent makes for it and a power down empties;
  hooks get it as `NBT_TMP_DIR`), under `/tmp` and `/var` outside the agent's folders, or one file named in full in
  `/dev/shmem` (no wildcard and no folder: other programs keep their shared objects there). A path of the right
  shape in a folder the agent does not know is skipped, not refused: the package installs and that log is not
  listed, so a package can name a log in a folder only a newer agent knows. `nbtpkg.py` refuses such a path to the
  package's author (`spec/log-paths.vectors` has the cases). The agent's own logs are those of `org.nbtapp.agent`.
  `files.list` lists them, regular files (and QNX shared memory objects, which a file in `/dev/shmem` may be) only
  and none reached through a link, and `files.fetch` sends one as a raw deflate stream (or as it is), a frame at a
  time while the link keeps up; an agent from 1.3.0 has these and lists the capability `files` in `hello`. `nbtctl
  logs ID` lists them, and `nbtctl pull ID PATH --out FILE` fetches one (`agent/log_files.hpp`, and the plan
  document's Message reference).
- A controller can show and change a package's settings. A package names them in its manifest's `settings`: the
  file they are kept in, under `${data}`, and options of four types (bool, int, text, select). Their values are
  lines `KEY=value` in that file, which the package's programs read, and which they or a person may write too:
  the agent keeps no copy, reads the file for every `settings.get`, and `settings.set` changes only the lines of
  the keys it is given, after it has checked every value against the manifest, leaving comments and every other
  line as they stand. A value the option's type does not take is shown as set by hand. A folder or file on the
  way that is a link is not gone through. An agent that has these lists the capability `settings` in `hello`,
  and needs a link whose frame holds 4,096 bytes. `nbtctl settings ID` lists them, `nbtctl set ID KEY VALUE`
  changes one and `nbtctl unset ID KEY` takes one out of the file (`agent/settings.hpp`, `spec/settings.vectors`
  for the value rules, and the plan document's Message reference).
- A controller can open a channel to a package's program: messages pass both ways between the phone and the
  program, through the agent, which reaches the program at a local socket and may start it. A package names its
  channels in its manifest's `channels`; a switched one is off until a phone or `nbtctl` switches it on. An agent
  from 1.7.0 has them and lists the capability `channels` in `hello`. See [A package's channels](#a-packages-channels).

`agent/hu_probe.sh` collects those answers from a head unit: which node it is, what is mounted where and what is
read-only, the room in `/var/opt` and `/tmp`, the plugin folders and their manifests, the tools a hook can count on,
the usage text of the program that restarts the HMI, and where the shared libraries are that the agent's programs
ask for. Run with no argument it changes nothing; `--exec-test` is a second run that copies a small stock program
into `/tmp` and `/var/opt`, runs the copy and removes it, to see whether programs start from there. Its header says
what it reads. The commands are under [The agent on the head unit](#the-agent-on-the-head-unit).

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
again; `make agent-test` does that itself. The image also holds the SSH package's programs and an OpenSSH client
(see [The SSH package](#the-ssh-package)): `make bench-image` exports their sources first (`make ssh-src`), which
fetches the two submodules the first time, so it needs git, patch and the network once.

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
make agent-test                                            # agent/ in the bench container (docker), one part after another
make -j4 --output-sync=target agent-test                   # the same, four parts at a time, as build.sh agent runs it
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
folder, and its manifest places the two programs, the launcher and the starter and does nothing else.

### A package's settings

A package whose program has settings can name them in its manifest, and a user then changes them in the phone
app, on the package's own screen, instead of editing a file on the head unit. This is what a package's author
does for that.

The manifest's `settings` names the file and the options:

```json
"settings": {
  "file": "${data}/chime.conf",
  "options": [
    { "key": "CHIME_ON", "label": "Chime", "type": "bool", "default": true },
    { "key": "CHIME_SECONDS", "label": "Chime length", "description": "How long the chime sounds.",
      "type": "int", "min": 1, "max": 30, "unit": "s", "default": 5, "applies": "app-start" },
    { "key": "CHIME_TUNE", "label": "Tune", "type": "select", "default": "low",
      "values": [ { "value": "low", "label": "Low" }, { "value": "high", "label": "High" } ] },
    { "key": "CHIME_LOG", "label": "Log file", "type": "text", "maxLength": 120,
      "unset": "No log file", "applies": "app-start", "advanced": true }
  ]
}
```

- `file` is under `${data}`, the package's own folder that an upgrade keeps (`${data}/settings` when it is left
  out).
- Each option has a `key` (capital letters, digits and `_`), a `label` and a `type`: `bool`; `int` with `min`,
  `max` and an optional `unit`; `text` with `maxLength` in bytes and `characters`, `safe` (letters, digits and
  `. _ : / @ + , -`, the default) or `any`; `select` with 2 to 16 `values`. A `description` says more.
- `default` is what the program does when the file has no line for the key, and the phone shows it. When there is
  no one value to show, because the program works it out, `unset` gives the words the phone shows instead, such
  as "From the car". An option has one or the other, or neither.
- `applies` says when a change takes effect: `now`, `app-start` (when the program next starts), `hmi-restart` or
  `reboot`. It is a promise the program keeps: the agent writes the file and restarts nothing.
- `advanced` puts the option under a folded section on the phone.
- At most 64 options. One option's strings together hold at most 1,024 bytes, and a value 255.
  `spec/manifest.schema.json` has every rule, and `nbtpkg.py build` checks them.
- A package that uses a later format of `settings` (a type, an `applies`, a `characters` rule or a limit that a
  later agent adds) sets `requires.agent` to that agent's version: an older agent refuses it with `agent-too-old`
  instead of as a bad manifest, and an older phone app still installs it, showing what it cannot read as read-only.

What the agent does with the file:

- It keeps no copy of the values. It reads the file each time the phone opens the package's settings, and when
  the user saves, it checks every value against the manifest, then changes the lines of those keys only and puts
  the whole file in place at once. Comments, blank lines and keys the manifest does not name stay as they are,
  so the file can hold more than the phone shows, and a person may still edit it by hand.
- A line it writes is the key, `=` and the value, with nothing around it: no quotes and no escapes. A `bool` is
  `1` or `0`; it also reads `on`, `yes` and `true` and their opposites, in any case. Reset takes the key's line
  out of the file. When a key is on several lines, the last one counts, and a save leaves one.
- The file is not there until the first change. A missing file means every option takes its default.
- A value the option's type does not take, written by hand, is shown on the phone as set by hand, and left as it
  is until the user gives another or resets it.

What the program does:

- It reads the file from the package's data folder: `/var/opt/nbtapp/data/<id>/` on a car, `$NBT_DATA_DIR` in a
  hook. It reads it again each time it needs the values, since a save puts a new file in place of the old one.
- It reads each value as plain text, line by line. It never runs the file with `.` or `source`, never `eval`s a
  line, and quotes a value it puts into a command (`"$value"`): a value is whatever a user typed. In a hook, with
  what a head unit has:

  ```sh
  chime_on=1 chime_seconds=5                        # the manifest's defaults
  file=$NBT_DATA_DIR/chime.conf
  cr=$(print '\r')                                  # a carriage return; this ksh has no $'\r'
  if [ -f "$file" ]; then
      while IFS= read -r line || [ -n "$line" ]; do  # a last line with no newline too
          line=${line%$cr}                          # one carriage return at the end goes, as for the agent
          case $line in
              CHIME_ON=*) chime_on=${line#CHIME_ON=} ;;
              CHIME_SECONDS=*) chime_seconds=${line#CHIME_SECONDS=} ;;
          esac
      done < "$file"
  fi
  ```

- It checks a value as the agent does and takes the default for one it does not understand, because a person may
  have written anything there. A line the agent writes is at most 320 bytes (a key of 64, `=`, a value of 255);
  a line written by hand may be longer, or end in a carriage return. A program with a fixed-size buffer copes
  with both.
- Its defaults are the manifest's. The agent cannot check that they agree.
- The package does not place the settings file with `install.files`: an upgrade would put the shipped one back
  over the user's. If it must ship one, it marks it `config: true`.

A package with `settings` installs on any agent, as older ones ignore the key. Its settings appear in the phone
app once the head unit's agent lists the capability `settings`. They live in `${data}`, so they last through an
upgrade, and through a removal unless the user has the package's data removed too. A new version that drops an
option leaves its line in the file, and one that changes an option's type may find the old value shown as set by
hand.

### A package's channels

A channel carries messages between a phone (or `nbtctl`) and a program of a package on the head unit, through the
agent. This is what a package's author does for one, and what the agent does with it. As yet a channel is for
NBTApps itself only: no other phone app may open one.

The manifest's `channels` names 1 to 8 of them:

```json
"requires": { "agent": ">=1.7.0" },
"channels": [
  { "name": "ssh", "apps": [], "access": "both", "switch": true,
    "start": { "program": "bin/relay", "args": ["--quiet"] } }
]
```

- `name` is 1 to 32 of `a-z`, `0-9` and `-`, the first a letter or a digit, none twice in a manifest. The program
  listens on a local stream socket of that name in `${tmp}/channels`, which the agent makes for the package, for
  its own user only. The package's id and the name have to leave that path short enough for a local socket on the
  head unit (`/tmp/nbtapp/apps/<id>/channels/<name>`); a package whose path is too long is refused at install.
- `apps` is the phone apps other than NBTApps that may open the channel. This agent knows none, so it is `[]`; an
  entry that names one, or has a key this agent does not know, is refused as `unsupported-manifest`, so that a
  package that uses such a thing names a later agent in `requires.agent`.
- `access` is `both`, or `read`, where the phone only reads: a message from it closes the channel.
- `switch`, when true, keeps the channel off until a phone or `nbtctl` switches it on (`channel.set`). The switch
  is a file in the agent's run folder, on the RAM disk: it is off again after a power down, it stays through a
  restart of the agent, nothing is written to flash for it, and it goes with its package. The iDrive plugin may
  not switch it. Switched off, its channels close (`off`), and an open still waiting for its program is refused as
  `off`.
- `start` names a file the package places in `${app}` with `install.files` (not a link), and up to 16 arguments.
  When a channel is opened and nothing listens, the agent starts it (see below). A package with one is flagged
  `starts-program` before it is installed.
- An agent before 1.7.0 ignores `channels` and would install the package without them, so `nbtpkg.py` asks for a
  `requires.agent` of 1.7.0 or later. A pre-release agent, 1.7.0-beta.1, is outside `>=1.7.0`: a package for it
  names `>=1.7.0-beta.1`.

What the program does:

- It listens at `$NBT_TMP_DIR/channels/<name>`, removing a file an earlier run left there first, and takes a
  connection for each channel that is opened: the agent connects once per channel, and never waits for it.
- On each connection the agent first writes one line of JSON and a newline:
  `{"v":1,"channel":"ssh","access":"both","caller":"nbtapps","phone":"Pixel 9","maxMessage":4096}`. `caller` is
  `nbtapps` for a phone, with the phone's name, or `cli` for `nbtctl`. After that each message, either way, is two
  bytes of length, high byte first, and 1 to 4,096 bytes. A message of another length closes the channel.
- Closing the connection closes the channel; the phone is told `closed` once what the program sent has gone to it.
  When the phone closes the channel, or its link goes, the agent closes the connection.
- The agent takes from the socket only as fast as the phone takes from the agent, and passes on at most 16
  messages from the phone that the program has not read: a program that reads slowly holds the phone back, and
  one that writes faster than the phone reads waits in its own socket. A stream of bytes, cut into messages and
  joined again on the other side, goes through as well, at whatever boundaries.

What the agent does:

- **Starting.** With `start`, when nothing listens, the agent starts the program once, in `${app}`, in a process
  group of its own, with standard input from `/dev/null`, its output and errors appended to
  `${tmp}/channels/<name>.log`, and the environment a hook gets: `PATH`, `NBT_PKG_ID`, `NBT_PKG_VERSION`,
  `NBT_APP_DIR`, `NBT_DATA_DIR`, `NBT_TMP_DIR`, and `NBT_CHANNEL`, the channel's name. It tries the socket every
  100 ms for 5 seconds, and `channel.open` waits for that; another open meanwhile starts nothing. A program that
  has not taken the connection by then is ended, and the open is refused as `unavailable`. The agent does not end
  a program when its channels close: it ends by itself, when it likes.
- **Its pid** is kept in the run folder (`channel-pid.<id>.<name>`), so an agent that is started again while the
  program runs knows it, as long as that process still runs that program: its executable has the program's file
  name, or, for a script, the process is the interpreter its `#!` line names and still leads the process group it
  was started in. A script that `exec`s a program of another file name is not known again.
- **Ending it**: SIGTERM to its group, and SIGKILL 2 seconds later if it is still there, without the agent waiting
  meanwhile. The agent does so when its channel is switched off, and before a job installs, upgrades or removes
  its package.
- **Its log** is kept under 256 KB while the agent runs: past that it is copied to `<name>.log.1`, over the one
  before, and emptied, and the program writes on at its new end. When the copy fails (a full RAM disk) the log is
  emptied all the same, and what it held is lost. Name both in the manifest's `logs`
  (`${tmp}/channels/*.log*`) for a phone to pull them.
- **Jobs.** While a job runs (from `job.start` until it has ended) every channel is held: the phones with channels
  are told `channel.paused`, nothing is read from or written to a program, nothing is opened, and when the job has
  ended they are told `channel.resumed`. An open still waiting for its program when the job starts is refused as
  `busy` then, and a program that open started is ended, unless another channel of it is open. Before its first step, the channels of a package the job installs,
  upgrades or removes are closed (`package-changed`) and the program the agent started for it is ended; the step
  waits for that program to go, or for 3 seconds. A new version's program is started by the next open.

The messages are in `agent/agent.hpp` ("Channels") and the plan document's Message reference. `nbtctl` opens a
channel from the head unit's shell or the bench:

```sh
nbtctl channel status org.nbtapp.ssh ssh      # switched, on, and how many connections it has
nbtctl channel on org.nbtapp.ssh ssh          # and off
nbtctl channel org.example.feed news          # each message as a line; --hex for its bytes in hex
nbtctl channel org.example.echo echo --send hello
nbtctl channel org.nbtapp.ssh ssh --raw       # standard input and output as the stream, for a proxy command
nbtctl channel org.nbtapp.ssh ssh --raw --send '{"v":1,"keys":[...]}' --skip-first   # the answer to --send on standard error
```

`nbtctl channel` ends when the channel closes, with 0 if its program closed it and 1 for another reason, with 3
when its link to the agent ends, and at Ctrl+C, SIGTERM or SIGHUP with 0, closing the channel itself. With `--raw`
the end of its input does not end it: it stops reading and goes on writing what the program sends, until one of
those ends (an SSH client ends its proxy command with SIGHUP). When its output cannot be written, because the
reader has gone, it closes the channel and exits with 1. Its output is the channel's alone, in every mode: a job
run meanwhile from another controller prints nothing there, and a line on standard error says when the job holds
the channel and when it goes on. With `--raw`, `--skip-first` takes the first message that comes out of the stream
and writes it to standard error as one line (each line end in it a space): with `--send`, that is how an SSH client's
proxy command reaches the SSH package's relay, which answers the keys message with its host keys before the server's
bytes begin (see [The SSH package](#the-ssh-package)). Without `--raw` it is refused: there the first message is a
line already.

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
A head unit that never had a plugin has no `Plugins` folder: the agent (1.5.1 and later) makes it with the
plugin's own folder, and takes it away again when the plugin is removed and nothing else is in it.

```sh
QNX_LICENSE_DIR=/path/to/qnx/license ./build.sh        # also leaves release/pkg/org.nbtapp.plugin-<version>.nbtpkg, not signed
make plugin-release KEY=/path/to/package.key           # -> release/pkg/signed/, and the SHA-256 the catalogue records
```

- **Its version** is the plugin's own, in `src/version.hpp`, and is not the agent's. A version that has been
  released is never built again with other content: raise the number.
- **`requires.agent`** in `plugin/package/manifest.json.in` names the oldest agent the plugin runs with. It is
  1.1.0, the first agent whose update brings its launcher: the plugin starts that launcher's starter. The
  **Reset Bluetooth link** screen needs an agent with `serial.reset`, and the **Stop the agent** screen one with
  `agent.stop`, and neither raises it: with an older agent the screen says that the agent cannot do this ("The
  agent cannot do this. Update it"), and the rest of the plugin works as before. When it is raised,
  and in which order the two packages are released, is under
  [Releasing the agent and the plugin](#releasing-the-agent-and-the-plugin).
- **`make plugin-release`** refuses what `make agent-release` refuses of the agent's package: a library that was
  not built from the commit checked out, or does not say it is the version of the package, and a key that is not
  built into the agent.
- **A plugin that was copied onto a head unit by hand** is in the way: the agent writes over no file that it has
  no record of, and the install fails with `stock-mismatch`. Move that folder out of `Plugins` once, as shown
  below for an update by hand, and install the package.

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

## The SSH package

`org.nbtapp.ssh`, "SSH access", is a root shell and file copy on the head unit over SSH, reached only through a
paired phone's link to the agent, and only while its switch is on. It is a package of its own, beside the agent's
and the plugin's: nothing of it is in the agent, which knows nothing of SSH. It holds three programs in
`${app}/bin/`: Dropbear's `dropbearmulti` (the server, its key tool and `scp`, with `${app}/bin/scp` a link to it),
OpenSSH's `sftp-server`, and `sshrelay`, ours; and in `${app}/licenses/` the licence texts of the first two, with a
README that says which program each belongs to. Its manifest is `ssh/package/manifest.json.in`; it runs no hook,
places nothing outside its own folders and asks for no restart, so its one risk flag is `starts-program`.

```sh
QNX_LICENSE_DIR=/path/to/qnx/license ./build.sh ssh    # also leaves release/pkg/org.nbtapp.ssh-<version>.nbtpkg, not signed
make ssh-release KEY=/path/to/package.key              # -> release/pkg/signed/, and the SHA-256 the catalogue records
```

- **Its version** is the package's own, `NBT_SSH_VERSION` in `ssh/relay/version.hpp`, and is neither the agent's
  nor the upstream versions. `sshrelay --version` prints it with the build, `git describe --always --dirty`, and
  the relay's log begins with that line. It is raised whenever anything the package holds changes, Dropbear or
  OpenSSH alone too: a version that has been released is never built again with other content.
- **`requires`**: agent `>=1.7.0-beta.1`, the first with channels (a pre-release agent is outside `>=1.7.0`), and
  app `>=0.9.0`.
- **`make ssh-release`** refuses what `make plugin-release` refuses: a relay that was not built from the commit
  checked out, or from a checkout with changes, or does not say it is the version of the package; a key that is
  not built into the agent; a signed archive of that version that is there already with other bytes. Dropbear and
  `sftp-server` hold no version of ours: they are tied to the checkout by what they are built from, the commits
  the submodules are pinned at and our patch, options and build script, which `make ssh` records in
  `build/ssh/qnx/sources`, and the release refuses programs whose record is not of the checkout.
- **Its channel** is `ssh`: for NBTApps itself (`"apps": []`), both ways, switched, and started on demand with
  `bin/sshrelay` as its program (see [A package's channels](#a-packages-channels)). The switch is off after every
  power down, and the iDrive plugin may not turn it on. `nbtctl channel on org.nbtapp.ssh ssh` turns it on from the
  head unit's shell or the bench.
- **The relay.** For each connection the agent makes to it, it reads the phone's first message, the public keys
  that may log in (`{"v":1,"keys":["ssh-ed25519 AAAA... comment", ...]}`, held to a strict rule: no options, two
  key types), writes them to a keys folder of that connection's own, starts `dropbear -i` on a socket pair for it,
  answers with the host key (`{"v":1,"hostKeys":["ssh-ed25519 AAAA..."],"pty":true}`, or `{"error":...}`), and
  then carries the SSH stream both ways in messages. It starts the pty driver a shell needs when there is none
  (`/bin/devc-pty`), serves four connections at once, and when a connection ends, ends what its server left
  behind. Logins are by public key only, as root, with no port forwarding; 30 minutes without traffic ends one.
  The whole of it is in `ssh/README`, "The relay".
- **What it writes where.** The host key is made on the head unit the first time the relay starts, and kept in
  `${data}/hostkey_ed25519`, on flash, until the package's data is removed (an upgrade keeps it; so does a removal
  that keeps the data), with `hostkey_ed25519.pub` beside it, the key tool's copy of the public line, which nothing
  reads. Everything else is on the RAM disk, in `${tmp}`: the relay's socket, the keys folders, its
  state file `sshrelay.state`, and the channel's logs `${tmp}/channels/ssh.log` and `ssh.log.1`, which the manifest
  names for a phone to pull.
- **An SSH client beside `nbtctl`** (on the bench, say) reaches the relay with `nbtctl` as its proxy command:
  `nbtctl channel org.nbtapp.ssh ssh --raw --send KEYS --skip-first`, where `KEYS` is the keys message; the relay's
  answer comes on `nbtctl`'s standard error, and the stream on its standard output.
- **The bench form.** The test bench's image holds the three programs built for it, in `/opt/nbtagent/ssh/`
  (Dropbear and `sftp-server` from the same exported sources and options as the head unit's, the relay without its
  test hooks), and Debian's OpenSSH client. `tests/packages/ssh-1.0.0` (and `ssh-1.0.1`, for an upgrade) is the
  package as the bench has it: the same id, channel, switch, program, logs and requires, with scripts in `bin/`
  that run the image's programs (`bin/scp` a script that runs `dropbearmulti` as `scp`, where the real package has
  a link), signed with the test key. `make ssh-e2e` (`tests/e2e/run_ssh.py`, part of `make agent-test`) installs it
  through the agent in its container and logs in with the image's `ssh`, `scp` and `sftp` through `nbtctl`: a
  command, a shell on a pty, files both ways, a wrong key and a wrong host key refused, the switch off and an
  upgrade with a session open, the agent started again while the relay runs, and removal. A phone's tests can
  install the same package in the bench.

## The agent on the head unit

`release/agent/` holds `nbtagent`, `nbtctl`, `version` and the scripts `hu_probe.sh`, `hu_install.sh`,
`nbtlaunch.sh` and `nbtstart.sh`. What they need of the head unit:

- A USB stick is `noexec`, so the programs are copied off it before they are started. The lines below copy them to
  `/tmp`, which is emptied when the head unit powers down.
- The programs ask for `libsocket.so.3`, `libcpp.so.4` and `libc.so.3`, as the Bluetooth tools do. The
  probe lists them, and `nbtagent --version` shows at once whether they are there.
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
maker's phone apps do not work over Bluetooth. If it cannot be set up (the gate is not in front, or the car's
service library cannot be loaded), the agent says why in its log, goes on with the local socket, and tries again
every 30 seconds, saying why again only when the reason has changed. If the car's Bluetooth service has not
attached 10 seconds after the agent asked the service broker for it, the log says so with the version of its
interface the agent asks for (`BSS_GAP.SGBluetoothGap 35.0`; the broker attaches only a server of the same major
version and a minor version as high or higher, and tells the client nothing otherwise), followed, once, by the
broker's line from the system log (`sloginfo`) that names the version the car has, or why there is none: sloginfo
not there, not runnable, ended by a signal, with another status or after 4 s, or, when it ran, how much of the log it
read (an empty or short log may have no such line any more). If the car's Bluetooth service
does not take the registration, or the service is not registered for 20 seconds (an agent that starts with the
head unit may be there before the car's Bluetooth service is), the agent says which it was (never attached,
attached and no answer to the registration, or refused), takes the gate away again, and sets both up anew 30
seconds later, and so on until the service is registered. A dial of the car's that is out then is given up first
("The car dials the phone", below), so that the dials go on as before once the service is registered again. A
client of the car's Bluetooth service that fails once it has been made (no memory for it, or its connection thread not started) cannot be opened again: the agent
says so once and Bluetooth serial stays off until the agent is started again.

**When the agent registers.** The car's Bluetooth stack advertises the first four serial registrations of this kind
that it holds, in the order they were made; a fifth is taken and answered as any other, but is in no service record,
so no phone finds it. The car's own software holds one to three of them, made within about a second of the stack's
opening, and one of those has an empty advertised UUID: a registration of that kind is mishandled by the stack at
its removal, which then leaves one entry of a table taken until the stack's process restarts, and that happens at
every switch-off that follows a switch-on at which another program registered before the car's own. So the agent
makes no registration of that kind, and none of its own before the car's are there. The agent asks the car's
Bluetooth service for its Status (on or off) and its Bluetooth name as it attaches, and sends no registration of any
kind until Status says the car's Bluetooth is on and has said so for 10 seconds (`--register-hold <s>`, below); when
it is on as the agent attaches, the 10 seconds count from the attach. A Status that is neither on nor off counts as
none, and a service that says nothing of Status in 10 seconds from the attach has the registrations go then, as
with no hold. Meanwhile the service is not registered: the agent takes and decides on the links phones open as
ever, the pairing window's dials and the dials of a version on trial wait, a knock waits for its dial as it does
while the service is not registered (4 seconds at most, and is then ignored with a line in the log), and the 20
seconds the agent gives the registration do not count. Switching Bluetooth off drops every registration, and the
car's Status says so: the agent marks its own and its stand-ins (below) gone, makes no registration and no check
until Bluetooth is on again and has been for the hold time, and then registers as at its start; each time it is
registered again, the services of other programs are swept again. An answer to a registration that went out before
the switch-off is passed over when it comes (15 seconds at the most), and the next registration waits for it. The
agent's own registration answered 1 (TIMEOUT), which the stack may have carried out all the same, is removed by name
and made once more, as a first refusal of any kind is. A car whose Bluetooth says nothing of a switch-off is met by
the registration check: while the service is registered, no link of the agent's is up and no phone has asked for a
link for 30 seconds, the agent checks it every 30 seconds by registering it again (`--register-check <s>`, below),
after its stand-ins: the car answers that it has it already, which changes nothing and is only counted, or takes it
anew, which the log says (`the car's Bluetooth had dropped the registration of 'NBTApp' ... and has registered it
again`). If the next check finds it gone again, one more line says that it keeps being dropped, and after that it
is only counted until a check finds it there or the stack busy (1 or 2: the next check, a registration of the same
name, finds it there or makes it). Any other answer is logged when it differs from the last such answer and only
counted while it repeats. A check never removes the registration first, as the agent's start does with one that is
refused. When it stops, the agent logs how many checks it made and what they found, with the last other answer and
how many times in a row it came, and for each stand-in how many of its registrations went and what they found.

The log has, with the time of each line, every change of Status (`the car's Bluetooth has been switched off (Status
0) ...`, `... has been switched on (Status 1): no serial registration goes out for 10 s ...`), every name the car
gives its Bluetooth (`LocalName`, with the number of its characters and never the name itself; '' at a switch-off),
the line that the registrations go out now and how long after Bluetooth was seen on, and each registration and its
answer: the 60 first lines about Status and the name in a run, and the rest only counted. The car's own software
sets the name at every switch-on just before it makes its own registrations, so the gap between the Status line,
the name lines and the agent's registrations is how long the car took. Its lines in the log start with
`bluetooth:`. Besides the phones'
requests for a link, the
agent asks the car's Bluetooth service for what it says of the car's device list, of links that end and of a
phone's services; it keeps that, and logs the first eight links that end, never with a phone's name or address.
When the car refuses a phone's link that may be the agent's, the first time and every 20th time, the agent asks
the car for its device list and logs what it says of that phone (not in it, or its connection state and its
security value), whether a serial node is there already (the car takes one phone-opened serial link at a time),
and then the other causes in the order to look at them: the iDrive Bluetooth pairing menu is open, the ignition is
off, the phone's pairing with the car is broken or missing, the car's coding does not allow serial links for
phones. Where the list shows the phone connected with no service connected (connection 2), the line also says that
the car has no media audio connection to the phone, that the car refuses the phone's own serial links while it has
none, so that the phone's app cannot knock meanwhile, and that the driver can connect it again by choosing the phone
in the car's media menu; `serial.status` counts
those lines (`refusalsNoAudio`).
The phone's Bluetooth pairing with
the car is not enough on this link: until the phone has been paired with the agent and has proved it with `auth`,
the agent answers `hello`, `ping`, `auth` and `pairing.issue`, and refuses everything else. A link that does not open with the protocol's magic bytes is let go (see "Other programs' links"
below), and one that opens with what an earlier link left behind is looked at past it (see "What the node does"). One that does, and then does not say `hello` within 30 seconds or goes silent for as long, is ended. With
`--root` and `--bluetooth` a dropped SSH session stops the agent as `kill` does, so that the registration does
not stay behind:

```sh
/tmp/nbt/nbtagent --root /tmp/hu --bluetooth 2> /tmp/hu.log &
/tmp/nbt/nbtagent --root /tmp/hu --bluetooth --decide 800 2> /tmp/hu.log &   # see "Other programs' links"
```

Once Bluetooth serial has started and its registration has gone out (or has not, after 15 seconds), a thread of
its own puts the start-up report into the log, as lines of `bluetooth: start-up report: ...`, within 30 seconds and
saying how long it took: the stack's version strings (asked with VersionGet, and logged when the reply comes), the
sizes of `libbssservice.so`, `libdsiservice.so` and `libpal_core.so` in `/opt/conn/lib`, the first line of
`/opt/sys/etc/nbt_version.txt`, the `#Coded is:` and `Bluetooth.Services` lines of
`/var/opt/conn/pss_config.cfg`, which of `/dev/btspp0` to `7` are there, and what the checks for software put on
by hand find (their ids and counts; nothing is fixed). A part that cannot be read is one line, and nothing in it
holds the agent up. Which program holds a serial node open is not in it: QNX 6.5 can only tell that by asking
every process for its open files (`pidin fd`), which sends a message to the server behind each of them.

The phone app then asks for a pairing code. The plugin's **Pair a phone** screen shows one; so does `nbtctl`,
which needs nothing of the HMI:

```sh
NBT_AGENT_SOCKET=/tmp/hu/agent.sock /tmp/nbt/nbtctl pair            # prints "code NNNNNN", then "paired"
NBT_AGENT_SOCKET=/tmp/hu/agent.sock /tmp/nbt/nbtctl phones          # the paired phones
NBT_AGENT_SOCKET=/tmp/hu/agent.sock /tmp/nbt/nbtctl forget --all    # end every pairing
```

**What the node does.** What the phone sends waits in the car's Bluetooth stack, ahead of `/dev/btsppN`, and
moves into the node only when new bytes come while nothing waits there, or when a `read()` reaches the node.
`select()` does not ask the stack, so it says that the node is empty while bytes wait ahead of it. Bytes that
nobody read outlive their link and come on a later link, ahead of that link's own, as one run of bytes. A node
that the agent had stopped reading while the phone went on sending was seen to leave every later link silent to
an agent that waited on `select()`, until a plain read of the node brought all the old bytes at once. A read that
does not wait, made while the node's queue is empty, takes nothing and has the stack hand on what waits ahead of
the node; after one read on a link, what the phone sends comes into the node as it comes, for a reader that waits on
`select()` too. Bytes that were read can be put back into the node's queue (`tcinject()`), and the next reader gets
them first, provided a descriptor stays open on the node: its last close empties the queue, which holds 6144 bytes.
So:

- every open node has one reader (`agent/node_reader.hpp`), a thread that issues a blocking read from the moment
  the node is opened, under a time limit of 100 ms that the kernel holds it to (`TimerTimeout`), and another one
  as soon as each comes back, until the node is closed. Nothing waits on a node with `select()`. The log says
  once per run whether such a read came back on time when its time was up, and, if the time limit could not be
  set, that it could not, and why;
- whole frames of the protocol at the start of a link are what an earlier link of the agent's left: the agent
  drops them, says how many bytes they were, and looks for the magic bytes after them, within the same 500 ms. A
  link whose leftovers are followed by anything else, or by nothing, may be another program's link with the
  agent's old bytes in front of it, and is let go as any other link that is not the agent's ("Other programs'
  links" below). A stale `hello` at the very start of the
  old bytes cannot be told from a new one: the agent takes it, meets the next stale magic bytes as a broken frame,
  and ends the link; its node is then read to the end, and the phone's next try is clean;
- a node is not closed under a link that may still carry bytes, except for a program that reads such links when
  what the agent read cannot be put back for it ("Other programs' links" below).

**How a link ends.** Closing a link's node does not end the link: the phone ends it, or the car does when it is
asked to. The agent therefore:

- tells the phone before it ends a link (`link.closing`, above), so that the phone app ends the link itself;
- when its own end of a link has closed, for whatever reason, keeps the link's node open and goes on reading it,
  dropping what comes, until the phone has ended the link, however long that takes. If the link is still up
  5 seconds after the agent's close (`serial.lingerMs` in `spec/protocol/constants.txt`), the log says so once. The
  link's last line in the log says how it ended, and how much the phone still sent:

  ```
  bluetooth: /dev/btspp0 closed, closed by the agent, and the phone ended the link 180 ms later: 5210 B from the phone, 3984 B to it
  bluetooth: /dev/btspp0: the link is still up 5000 ms after the agent's end closed (closed by the agent): its node is kept, and read, until the phone ends the link
  ```

- when it stops, or ends for a newer version, waits up to 6 seconds for the phones to end their links before it
  takes its service and its gate away and ends. A link that is still up then has its node closed with the process.
  `kill -9` leaves no time for any of it;
- has the car end the phone's link when it is asked to: `serial.reset`, which `nbtctl serial reset` sends. The
  agent sends the car's Bluetooth service a ServiceDisconnect for the phone that last asked for a link. With a
  serial node open (one being decided on, a link of the agent's, or one being read to its end) that goes out about
  300 ms after the answer, so that a phone that asked on that very link has its answer first; if by then no such
  node is there any more, the link has ended meanwhile, or the one being decided on has been let go to another
  program, and nothing is sent: the log and the problem file say which, and the reset is done. A link of the
  agent's counts whether or not Bluetooth serial was set up again since it came. A node the agent keeps open, unread, for another program ("Other programs' links") is not
  counted: the car would end that program's link. A ServiceDisconnect for a phone-opened link when the car has no live link
  there can hold that instance up for well over a minute. With none open, the
  next serial link whose node opens within 60 seconds is ended at the open, with no decide step, and its node read
  until the link ends; that is for a phone whose links come up and carry nothing, which taps Connect once more.
  One ServiceDisconnect is out at a time, as the car's answers say little of which request they answer: while
  another (one of the dial's, say) waits for its answer, this one goes once that is answered, or given up after 3
  seconds with none.
  The log says what the car answered (its error code and a reason), how long after the request the link's node
  went, or that the link was still up 3 seconds after it.
  The phone's address goes into the request and never into the log. Which values the car wants in the request is
  not settled; `nbtctl call serial.reset '{"device":"...","instance":"...","service":"..."}'` tries others without
  a new build:

  ```sh
  nbtctl serial reset     # "sent": the car was asked to end the link, and the agent's log says what came of it
                          # "armed": no serial link is up; tap Connect in the phone app within 60 s
  ```

  On the car's screen it is **NBTApp**, **Reset Bluetooth link**, and that page's second row, **Reset now**:
  the plugin sends `serial.reset` with no values over the agent's local socket, as **Forget all phones** sends
  `phones.forget`, waits up to 5 seconds for the answer, and the first row says what came of it:
  **Now tap Connect in the phone app** for `armed`, **Ending it. Connect again in the app** for `sent`, **A job
  is running. Try again later** for `busy`, and **The agent cannot do this. Update it** for `unsupported`, which
  is what an agent from before `serial.reset` answers, and one with no Bluetooth serial listener. One press per
  showing; the page starts over when it is shown again. It needs no SSH and no link from the phone.

A link that the agent lets go as another program's (below) is kept open, unread, for that program if there is a
sign of one, with what the agent read from it put back (or closed for it, where that cannot be done), and read
otherwise, until it ends or a sign of one comes.

**The car dials the phone.** The car gives every link a phone opens the one node `/dev/btspp0`, and takes one such
link at a time, so another program that holds it keeps the phone's app from the agent. The car can also open a
link itself, to a service on the phone: the agent asks the car's Bluetooth service to (ServiceConnect) on the car's
instance `nbtapp` of `BtCtrl`, for the phone's address and the agent's service, and that link's node is
`/dev/nbtapp`, which nothing else uses. The phone's app listens on the agent's UUID and accepts it. The link the
phone opens works as before beside it, except where a rule leaves it to another program ("Other programs' links"
below, with `--dial-knock off` only); `nbtagent --dial off` turns the dial off, and the agent then takes only the
links phones open, as before.

The car dials a phone only when its app has just said that it listens: it knocks (below, the default). The other
way, behind `nbtagent --dial-knock off` for comparisons, is the one from before the knock: the car asks the phone
for its service list before every dial and dials it when the list names the agent's service, at polls and at the
phone's own requests ("Ask first" and "When", below). That cannot work on current phones: the car fetches at most
a phone's first 15 service records, and an app's own record comes after them, so the list never names the app. A
dial without an ask is no way out either, since a dial that the app does not take can cost the phone its media
audio at the car ("The re-point", below).

- **The knock.** An agent that dials this way lists the capability `serial-knock` in `hello`, and a phone app
  knocks at such an agent, and once at one it has not greeted yet (the probing knock, below) (`serial.knock*` in
  `spec/protocol/constants.txt`). The app makes up to 5 connects
  to the agent's service, 250 ms after each that fails (one may fail on the phone while another app's link to the
  car is up); where one of them fails, the app makes its next connects to the car's standard Serial Port service
  (0x1101) instead, with the same bytes and hold: the car advertises only some of the serial services registered
  there, so the agent's may have no record a phone can find, and every serial record of the car names the same
  RFCOMM channel, so such a link reaches the agent as one to its own service does, and the agent takes the knock by
  its bytes whatever service the phone opened. It makes at most 2 such connects at a time, none after one of them
  came up (where no agent runs, another program at the car may take that link), and then goes back to the agent's
  service for the connects that remain, so a knock makes no more connects than it would without them. How often
  depends on who started the knock: for a connect the user starts, once in each burst (each later burst then begins
  at the Serial Port service); for the knocks the app makes by itself, once in the whole knock; for the app's wait
  after an agent's update, never. One that comes up carries the knock's four magic bytes, `NBTK`, and nothing else: the agent reads
  them within the decide step's 500 ms, writes nothing into the link, and lets it go, reading its node and dropping
  what comes until the app ends the link (it is never handed to another program), and takes it as the knock of the
  phone whose request brought that link. A phone's request for a serial link is no knock, whatever becomes of it. A
  connect that the car refuses reaches no agent, so where the car refuses a phone's links (it does so while it has
  no media audio connection to that phone) its app cannot knock; the refusal's log line says so, and choosing the
  phone in the car's media menu connects it again. A knock of any phone that is connected to the car and paired
  with it, one of the agent's or not, has the car dial it at once, with ServiceConnect and no ask, when no link of the agent's is up and no dial is out; one that comes while a
  phone's request is pending, a link is being decided on, another phone's dial is out or the re-point waits, waits
  for its dial 4 seconds at most. A knock that comes while that phone's dial is out or up, or its knock waits
  already, adds nothing: that is how the connects of a burst that come up one after another bring one dial. A dial
  a knock brought that gives the agent no link (any answer but 0, no answer, something that is not the app) brings
  the re-point as any failed dial does, and the phone's quiet time: its knocks are ignored for 3 seconds. It is
  not backed off and not retried: the quiet time takes the place of both. A phone the agent has forgotten is dialled
  for its knock too: `auth` on the dialled link is refused there as on a link the phone opens itself, and its app
  learns that it needs pairing. Besides knocks the car dials only: while
  the pairing window is open, every phone there in turn every 5 seconds; at `nbtctl serial dial`; a phone whose link
  left behind a dial has just ended; and while this version of the agent is on trial after an update and no link of
  the agent's is up, every phone of the agent's there every 5 seconds, so that an update is never left waiting for a
  phone that does not know it should knock, until a controller confirms the version, and for 2 minutes at most. The
  2 minutes count from the moment the agent knows it is on trial, its serial service is registered and the car's
  device list has come, so that a slow start of the car's Bluetooth takes none of them; once they are over, the log
  says that the car no longer dials by itself for this trial, and until the version is confirmed a phone is dialled
  when its app knocks, as at any other time, so that an update nobody confirms does not bring a failed dial every 5
  seconds for as long as the agent runs. None of these asks first; there
  is no poll and no ask. The log says each knock and what became of it; `serial.status` counts them.

The phone app listens while it is open, or busy with something that needs the link (a job, a pull of logs), with
Bluetooth on and no link, and not after the user's own Disconnect until they connect again or come back to the
app; a connect of its own listens too, beside the phone's dial, and takes whichever link comes up first. To an
agent that lists `serial-knock` a connect knocks in place of the phone's dial, and listens for the car's link; the
phone's own link is made only where the user asks for it (**Connect from the phone**), which the app offers where
a knock brought no link. To an agent the app does not know to knock yet, as one it has never greeted (a first
connect, pairing, the set-up), a connect knocks once first, a probing knock: one burst, ending after the first of
its connects that came up, while the app already listens for the car's link. An agent that knows knocks has the
car dial any phone that knocks and is connected to the car and paired with it, one of its own or not, so this
reaches it even where the phone cannot find the agent's service; its `hello` then tells the app to knock from then
on. Where no link comes, the app dials the agent's service as before. A connect from the phone and the app's wait
after an agent's update make no probing knock. Where
`info.get` said that the phone's own links are left to another program (`phoneOpenedLeft`), a connect makes no dial
and only listens, for 40 seconds, and the app offers the phone's own connect, once, when no link came; a link the
phone did open gives way to one the car opens while it waits for its answer. It listens on the secure kind of RFCOMM socket, so only a device paired with the phone gets through, and it takes a
link only from the Bluetooth address of a head unit it has saved: any other is closed at once. A link the car
opened is then the same as one the phone opened: the phone writes the magic bytes and `hello`, the agent answers
nothing but `hello`, `ping`, `auth` and `pairing.issue` until the phone has proved with `auth` that it holds a
pairing key, and nothing on the link is trusted before that.

- **Whom.** A phone that is connected to the car (the car's device list says connection 2 or more) and paired
  with it with its key (security 7 or 8), and that is one of the agent's: the agent keeps the Bluetooth address of
  the serial link on which a phone paired or last did `auth` in `phones.json`, in plain text, and nowhere else (not
  in an answer, not in the log, which names phones as "phone 1", "phone 2" by the order the car's list first had
  them). Once a link the car dialled to a phone has been the agent's (its app opened it with the magic bytes), the
  phone's entry there also says `"dialled": true`, for the rule in "Other programs' links"; an entry without it reads
  as not yet, and the mark goes when the phone's address changes or the phone is forgotten, and when 3 dials of the
  phone in a row have given the agent no link (an answer that may leave the car's record wrong, below, no answer, or
  something that was not the app): the log says so once, and the next dialled link the app takes marks it again.
  That run is counted in memory only. While the pairing window
  is open, every phone connected and paired with the car is dialled, the agent's or not, for a new phone's app to be
  reached. A phone that has lost its pairing with the car is never dialled: the
  car would start a new pairing, with a prompt on both screens. A phone that the car says is being paired anew or
  failed to pair, or whose security becomes 9, is not dialled until the device list shows it paired again after a
  change; a pairing that worked does not stop its dials. The mark `"dialled"` is kept with the knock too, and only
  the rule of "Other programs' links" with `--dial-knock off` goes by it.
- **Ask first** (`--dial-knock off` only). Before every dial, whatever brings it, the car is asked for the phone's service list
  (ServiceListQuery), and the phone is dialled at once only if the list names the agent's service, which the app
  publishes only while it listens. A list without it means the app is not listening: no dial, nothing held against
  the phone, and the next poll asks again. An error, a rejection, or no answer within 6 seconds (the car's BUSY to a
  query never reaches the client), counts as such a list, and holds nothing back either, however many come in a row:
  a phone whose app only listens cannot ask for anything itself, so the polls are its only way in. The log says so
  once when three have come in a row. A phone whose connection
  is below 2 is never asked: a query pages a phone that has no link to the car. Each ask takes the car about a
  second and a half and brings every client of its Bluetooth service three ServiceListChanged informs, which the
  agent only counts. An ask does no harm; a failed dial is what the ask is there to avoid: each one the car answers
  with a failure can leave its record of phone-opened serial links wrong, and it then refuses such links (another
  program's too) after one of them ends. So such a dial backs its phone off, below, and is not made again soon.
- **When** (`--dial-knock off` only; the knock's own times are above). One ask or dial at a time, and none while a link of the agent's of either kind is up or being decided
  on, or while the service is not registered; a link that comes, or a phone's request that becomes pending, ends an
  ask, whose answer is then passed over, and the next ask waits for that answer or the 6 seconds. A poll of the
  agent's phones as soon as the car's device list has come, then every 10 seconds for 2 minutes after a phone
  connects to the car or a link of the agent's ends, and every 30 seconds after that (`--dial-poll 10,120,30`). A
  phone that asked the car for a serial link and got no link of the agent's within a second is asked then, even
  while its request is still pending; that request holds back every other ask until its node has come or its time
  is over. A phone's requests bring one ask of it every 10 seconds at most, counted from an ask that went to the
  car (one that could not be sent does not count), and one more in that time, once, after an ask of theirs that
  another request ended before its answer came. No poll's ask, retry and ask a request brings comes within 8
  seconds of a phone connecting to the car (or of the first device list, for a phone there already): the car's own
  media component asks the phone for its services as its audio connects, and of two queries that meet, one fails;
  a poll comes as soon as that time is over, and the request's ask waits for it.
  While the pairing window is open, a round of every phone there every 5 seconds. `nbtctl serial dial`, over the
  local socket only, asks and dials the phones connected to the car and paired with it now, the agent's or not;
  neither it nor the pairing window waits for the 8 seconds.
- **What the car answers.** 0: the link is up, and the agent opens `/dev/nbtapp` and waits 3 seconds for the
  app's magic bytes (the app accepts the link before it writes); with them the link is the agent's, a serial link
  like any other, with no gate, hold or let-go. Anything else on it, or nothing, is something that answered on the
  agent's UUID and is not the app: the car is asked to end the link (ServiceDisconnect), and the phone is backed
  off: polls leave it alone for 30 seconds, twice as long each time it happens again, up to 5 minutes, until a
  dialled link of the agent's comes up for it or its connection changes, and so does its own request, whose ask
  waits until the back-off is over; the pairing window and `nbtctl serial dial` still ask it at once. 5: the phone
  did not take the link although its list named the agent's service just before, so its app may have stopped
  listening since, or something else went wrong: backed off. 14: the phone is not in reach: backed off. 2: a link
  on the instance is up already; with its node there, it is a link an agent that was killed left behind, which is ended and never taken, and the phone is dialled afresh; if the car
  does not end it, the phone is backed off instead, and is not dialled again at once; with no node there, as 10. 10,
  or a request the car's Bluetooth service did not take or that could not be sent: again in 15 seconds. Any other
  answer: backed off. No answer in 95 seconds: the dial is given up, the car is asked to end what may be up, and the
  phone is backed off; the next dial goes as usual. A dial that is out when Bluetooth serial stops, or is set up
  again, has 3 seconds more for its answer and is then given up the same way, but the car is not asked to end
  anything while the dial may still be out in it. The car's answer that comes after the dial was given up, while no
  other request of the agent's waits for its answer, is said in the log as late and is the answer to nothing else:
  a failure brings the re-point (below), and 0, a link up on `nbtapp` after all that nothing of the agent's reads,
  has the car asked to end it. A late answer that comes while the agent dials the same phone again cannot be told
  from that dial's own answer, and is taken as it. With the knock the answers are taken the same way, but for what
  comes after a failure: no poll comes and no back-off or retry is kept for the knocks; a phone whose dial a knock
  brought has its quiet time, and any other is dialled again when it knocks.
- **A dial that still fails.** A dial answered with anything but 0, 2 or 10, or not at all (with `--dial-knock off`:
  although the phone's list named the agent's service), may have left the car's record of phone-opened links wrong. The log says so once,
  the problem file gets a line, and `serial.status` says `"recordDoubtful": true` until a dial is answered 0, the
  car says that a phone-opened link (`ExtBox1`) ended, as it does when that record is right, or its Bluetooth service
  goes. The agent sends nothing to make that record right: a ServiceDisconnect for `ExtBox1` with no live link holds
  that instance up for well over a minute.
- **The re-point.** When the car answers a dial with such a failure, its Bluetooth service keeps an entry for the
  agent's instance `nbtapp` that still names the phone. The end of a later phone-opened link can then be booked
  against that entry, and the car counts one connected service too few for the phone; when that count reaches zero,
  the car closes the phone's media audio. So the moment the agent sees such an answer (also one that comes late, to a
  dial it had given up, above), it sends one more ServiceConnect on `nbtapp`, with an address that belongs to no device and
  a service the instance does not serve. The car answers it with 5 within some tens of milliseconds and sends nothing
  over the air; on the way it writes that address into the agent's entry, so a link end booked against the entry
  names an address the car has no record of, and the phone's count is left alone. The record stays doubted all the
  same. While its answer is waited for, 3 seconds at most, no ask or dial starts. An answer other than 5, a 5 that
  took half a second or more, or none in the 3 seconds is not what the car is expected to do: the log and the problem
  file say so once, and no more re-points are sent until the agent is started again; after
  an answer 0 the car is also asked to end what it may have connected for the request. `serial.status` counts them
  (`repoints`, `repointsExpected`, `repointsUnexpected`, `repointsNotSent`) and says `"repointStopped": true` once
  they have stopped.
- **How it ends.** The phone ends it, or the car says it ended (ServiceDisconnected for `nbtapp`), and its node is
  closed at once. When the agent ends it, it says `link.closing` first as on any link, gives the phone 5 seconds to
  end it, and then has the car end it with ServiceDisconnect, waits up to 3 seconds for the answer, and closes the
  node: unlike a phone's own link, it is not kept and read. `serial.reset` ends it the same way. When the agent
  stops, the dialled link is ended before the registration is removed, which the car refuses while a link is up.
  A link left behind on `/dev/nbtapp` when the agent starts is ended once the car's phones are known, with
  ServiceDisconnect for each phone connected to the car in turn until its node has gone (an error answer for a
  phone that has no such link does no harm), and then the agent's phones are dialled afresh (with `--dial-knock
  off`; with the knock they are dialled when they knock). Such a link also
  makes the car refuse the registration (5 to SppUuidRegister and to SppUuidDeregister alike): with `/dev/nbtapp`
  there when the registration is refused, the agent asks for the car's device list and ends that link the same way
  first, within 15 seconds, and registers again at once if it has gone. If it is still there, Bluetooth serial is
  off and set up again 30 seconds later as for any refusal, and the same is tried once on each new try.
- **A request the dial overtakes.** The car ends a phone's pending request for a link when its own dial to the
  phone comes up, and says so just before the dial's answer. A request ended while the dial is out, or within a
  second of its answer 0, is logged as such and is not counted as a refusal.
- **The log.** What an ask came to (the app not listening, no answer, an error, a rejection, a reply that could not
  be read) is said when it differs from that
  phone's last ask, and otherwise only counted, and the next line that differs says how many came; a list that names
  the service again after such asks is said once, with that count, and the dial follows. An ask that is ended (a
  link came, a phone asked for one, or the service is not registered) is said the same way by its reason, apart from the answers, so
  that ended asks between lists without the service bring no line for either. One line for each dial,
  when it is answered: the phone by its number, why it was dialled (poll,
  the phone asked for a link, pairing window, asked for, retried, afresh, the phone knocked, the agent is on trial), the car's
  answer by its name and how long it took. The same answer for the same phone again is only counted, and the next
  line that differs says how many times it came. A line each when the car's device list says a phone came, went,
  connected or changed its security. A clean-up of a link left behind that leaves its node there is logged in
  full the first time; after it, one that comes to the same is only counted, and the next that differs, or that
  ends the link, says how many came before it.

```sh
nbtctl serial status    # how Bluetooth serial stands: one "name value" line each, counts as counts.dials and so on
nbtctl serial dial      # "dialling N phones": the agent's log says what came of each
```

**The problem file.** The agent's log is on the RAM disk and is gone after a power down. What went wrong with
Bluetooth serial is also put, a short line each, into `serial-problems.log` in the agent's state folder
(`/var/opt/nbtapp`), which is on flash: a registration that switching Bluetooth off and on had dropped and the check
restored, the car's Bluetooth service that never attached, the car refusing a phone's link, a dial that went wrong
(an answer other than 0, 5 and 14, no answer, something that is not the app), a failed dial after the phone's list
named the agent's service, the car asked to end a link left
behind, and that link not ended, and
`serial.reset`, and one that sent nothing because the link had ended or had been let go to another program. Each line has the time, the agent's version
and build, and what happened, never a phone's address or name. Only the first problem of each kind in a run of the agent is written, and 16 lines a run at most; the
rest are counted, and `nbtctl serial status` shows both counts (`problems.recorded`, `problems.counted`). When a
line would take the file over 32 KB, its older half goes in one rewrite. A run with no problem does not create the
file, open it or touch it.

**Serial services of other programs.** The car's Bluetooth stack keeps a registration until someone removes it or
the stack restarts. A program that registers a serial service of its own and ends without removing it leaves it
behind: phones go on opening links to a service that nothing answers, and the car has one serial link for a phone,
so the phone's app does not reach the agent on a link it opens itself (a link the car dials, above, does not use
that node). The stack does not say what is registered, so the agent has to be told which services there are and
whose:

- a package says so in its manifest, with the name the registration is filed under and the file name of the
  program that makes it. Two installed packages cannot declare the same name:

  ```json
  "install": { "serialServices": [ { "name": "TestRadio", "program": "radiod" } ] }
  ```

- for a program that no package installed, a line in the file `serial-services` in the agent's state folder
  (`/var/opt/nbtapp`, or `state/` under `--root`): the name, a space, the program's file name. A line that starts
  with `#` is a comment.

The stack also advertises only its first four registrations ("When the agent registers", above), so a program
that registers after the agent, with the car's own ahead of both, may be in no service record, and phones never
find its service. So, with Bluetooth serial on, the agent keeps a place for each such service below its own: each
time it registers its own service (as it starts, after the car's Bluetooth service comes back, and after the car's
Bluetooth has been switched on again), it first registers a **stand-in** under each declared service's name, one at
a time in the order they are declared: a registration of that name, with a UUID made up for it (the first 16 bytes
of the SHA-256 of `NBTApps stand-in serial service:` and the name, marked as a UUID of version 8: the same for the
same name every time, and another for every other name) as both its advertised and its dialled UUID. A stand-in the
car takes holds the name and the place; one answered 5 finds the name there already (the program's own
registration, a stand-in from before, or no place left) and is left so; any other answer is logged, and the agent's
own registration follows all the same. The stand-ins go ahead of every registration check too, and there their
answers are logged only when one differs from that stand-in's last. While the program is not running a phone sees
a record under the service's name with the made-up UUID, which no phone looks for.

**What a declared program has to do.** Its registration is answered 5 while the agent's stand-in holds its name.
A program declared for a serial service must, when the car's Bluetooth service answers its registration 5 ("the
name is there"), remove the registration of that name (SppUuidDeregister) and register again, and so take the
stand-in's place; a program that registers once and gives up on a 5 stays without its service while the agent
runs. It must also register with an advertised UUID of its own, never an empty one.

The car's Bluetooth service cannot be asked what is registered under a name, so the agent keeps it: a name holds
its stand-in from the moment the car took the stand-in until its program is seen running, the car's Bluetooth is
switched off, the car's Bluetooth service goes, or the name is removed; at any other time what it holds is not
known. With Bluetooth serial on, the agent **sweeps** every declared service whose program is not running: when
its own service has been registered; during an install step once the hook that stops the package's programs has
had its turn (`preInstall` of an install or an upgrade, `preRemove` of a removal); and when it has let a phone's
link go, at most once in 30 seconds. A name that holds the agent's stand-in is left as it is, and the log says so
once until that changes. Any other name is replaced: what it holds, which may be a registration the program left
behind, which phones would open links to with nobody reading them, is removed by name, and the stand-in registered
in its place at once. A program that starts in between and registers keeps its registration: the stand-in is then
answered 5. It looks for the program among the head unit's processes by the file name of the executable, so a
program that a shell runs from a script is not found; it looks before every registration check too, and a name
whose program runs is not known to hold the stand-in from then on. A service whose program runs is never touched
but for the stand-in's registration in a round, which the car refuses while the program's own is there, and every
service is left alone when the processes cannot be read. While no registration may go (the hold, or Bluetooth off)
nothing is replaced, and the log says that the car's Bluetooth service cannot be asked now. Each removal and
registration, and what the car's Bluetooth service answered, is in the log.

**No place left.** The stack's answer 5 to the agent's own registration means "the name is there" or "no place".
The agent removes its own name then, and its answer 10 says that nothing was there; if the registration made once
more is refused too, while stand-ins of the agent's hold their names, the agent gives up stand-ins rather than stay
without its own service, and no more of them than it needs: it removes the last of them, in the order they are
declared, and registers its own once more, in the place that one left; refused again, it gives up the next-to-last
the same way, and so on, until its own is taken or no stand-in of its own is left (and then the registration has
failed, as before). The agent's own service needs no advertised place (the app reaches it by its knock through the
car's own serial service, and the car dials from any place), while a declared program does, so the first stand-ins
keep the advertised places for their programs. The log says, for each stand-in given up, which it was and that its
service has no stand-in for now, so that its program may be refused a place or get one that no phone finds; and,
once the agent's own is registered, which stand-ins still hold their names. A stand-in whose removal is answered
other than 0 or 10 is no longer counted as the agent's, and the agent's own is registered once more all the same.
A stand-in given up is not registered again then: it goes first again in the next round, the registration check's
or that of the next switch-on, where it is refused as long as no place is free; a place that has come free by then
it takes, after the agent's own, which may be the place the stack does not advertise. Switching Bluetooth off, or the
car's Bluetooth service going, in the middle of this ends it, and the next round starts afresh.

**When the agent stops, and when the list changes.** As Bluetooth serial stops (the agent stopping, or giving up
on its registration), after its own registration, the agent removes every stand-in that holds its name and whose
program is not running, waits up to 3 seconds for the answers, and says how many; a name whose program runs is left
alone. A service that is no longer declared (its package removed, or the file changed) loses its stand-in at once
when that holds its name, and its name is left alone otherwise. The agent keeps nothing of its stand-ins on disk: an
agent that starts knows only the services declared then, and a stand-in that an agent killed earlier left under a
name no longer declared stays until the car's Bluetooth is switched off or its stack restarts.

**Other programs' links.** The car gives every link a phone opens the same node and names the same service for
it, so a program that takes serial links of its own meets the agent's links, and the agent meets its links. Only
a phone's request that names the agent's service or the car's serial service is matched to a serial node; one for
another service (audio, hands-free) never brings one, is only logged, and does not have the next node left alone.
Three things keep the two apart, all in the agent:

- With the knock (the default) there is no such rule: every link a phone opens is read as below, whoever the phone
  is, since it may be that phone's knock ("The car dials the phone", above), and `phoneOpenedLeft` is always false.
  With `--dial-knock off`,
  a rule leaves a link a phone opens to the other program: the agent never reads it, and puts no hold on (below)
  for the phone's request that brings it, when all three of these hold at the request and again when the node
  appears: the car dials the phone (`--dial on`, the default); a program declared for a serial service (above) is
  running, or whether one is cannot be told; and the phone that asked is one of the agent's whose app a link the car
  dialled has been the agent's on before (`"dialled": true` in `phones.json`, "The car dials the phone" below, kept
  across restarts). It does open the node at once, and primes it for the other program: a program that waits on
  `select()` before it reads never sees what the phone sent on a link no read was made on ("What the node does",
  above). With nothing in the node's queue, the agent makes one read that does not wait, which as a rule takes
  nothing; bytes it does take, which came in that very moment, are put back. It then keeps the node open, unread,
  until the link ends, and closes it then; the node counts as no link of the agent's for anything else (the dial,
  the hold, `serial.reset`). A node that cannot be opened or primed is closed again and left as it is, and the log
  says why. The other program then gets the link whole, its first bytes too, which the decide step below would
  have read, and the agent's app is reached by the dial, which the request still
  brings. A link that the agent's app opens itself on such a phone is left all the same: that is the rule's cost.
  The mark is a safety net for a car on which dialling does not work: there the phone's own link stays the way in.
  So it is also given up when dialling stops working for a phone that has it: after 3 dials of the phone in a row
  that gave the agent no link, the agent clears it in `phones.json`, the log says so once, and the phone's own links
  are decided on as before the mark, with `phoneOpenedLeft` false for it, until a dialled link is the agent's again.
  In every other case (dial off, no such program, a phone that is not the agent's or that no dial has reached, a
  request that gave no address, a node with no request) the agent goes on as below, and so it does when the rule
  held at the request and no longer does when the node appears. The log says once for each link that it was left
  to another program, and why, and how the node was primed: with bytes put back or not; `serial.status` counts
  such links (`leftUnread`, whether the node could be primed or not), and the links on which bytes were put back
  here or in the decide step below (`putBack`). The phone's app learns that the rule
  holds for its phone from `info.get` (`"phoneOpenedLeft": true` in `serial`), and then opens no link itself:
  it only waits for the car's.
- The agent opens a link's node and reads what the phone says first. A link that opens with the protocol's magic
  bytes, after any leftovers of an earlier link ("What the node does", above), is the agent's. With the knock, one
  that opens with the knock's magic bytes there instead is that phone's knock: the agent writes nothing into it,
  reads its node and drops what comes until the link ends, and never hands it to another program. Any other is let
  go, at once if something else came first and after 500 ms if nothing came, and the agent has written nothing
  into it. When there is a sign of a program the link may belong to (a program declared for a serial service,
  above, is running, or whether one is cannot be told; the gate has seen another program open `/dev/btspp0`, or try
  to, since this link's node appeared, the car's media process, which the gate refuses every time, not counting; or
  a program of software put on by hand, the checks' `/opt/aa`, is running, which nothing declares), the agent stops
  reading the node and puts all it read back into the node's queue, in the order it came, with whatever came into
  the queue after its last read behind it, and keeps the node open, unread, until the link ends: that program finds
  the link whole, as it would have. A put-back is made only into a node that takes bytes as they are (raw) and
  only when the bytes fit in its queue; where it cannot be made, or fails, the agent closes the node for that
  program as before and the log says why: what the phone had sent by then is then lost to that program. With no
  such sign nobody would ever read the link, and what waited unread would come ahead of
  the next link's bytes: the agent reads the node, dropping what comes, until the link ends. If a sign comes while
  it does (the gate sees another program open the node, or try to, after the link was let go; or a program of
  software put on by hand starts to run, looked for every 2 seconds at most), the agent stops reading and closes
  the node for that program; what it had read and dropped by then is gone, and the log says how much. The log
  says which it was, and why.
- From a phone's request for a link until that link has been let go, and for as long as a link of the agent's is
  up, the gate also refuses the opens of the programs that are declared for a serial service (above), so that none
  of them takes a link's first bytes or writes into the agent's link. A phone's knock keeps the hold on until the
  phone ends the knock's link, with no time limit, so that none of them writes into it either; while that link is
  up the car has no other link a phone opened for them to take. Their looks at the node (`stat`, `access`)
  are passed on. A program that is not declared is not held off. A request whose link the rule above leaves to
  another program puts no hold on, so that program's opens pass at once; the car's media process is refused all
  the same.

The log has, for every link, how long after the open its first message came, how many bytes an earlier link had
left on it, whether it was the agent's or was let go, and why, how many bytes were put back for another program or
why they could not be, whether the node was still there after the close and when it went, how much was dropped
from a node that was read to its end, and each time the gate starts and stops refusing those programs. One option
of `nbtagent` changes the decide step without a new build, two the registration (its hold and its check), and three
the car's dialling; all six go with Bluetooth serial only:

| Option | What it changes |
|---|---|
| `--decide <ms>` | How long the magic bytes are waited for, 1 to 10000, in place of the protocol's 500 ms. A phone app goes by the protocol's time, so this is for measuring, not for a car that is in use |
| `--register-check <s>` | Every how many seconds the registration is checked while all is quiet, 0 to 3600, in place of 30; 0 never checks it |
| `--register-hold <s>` | How many seconds the car's Bluetooth has to have said it is on before a serial registration goes, 0 to 600, in place of 10; with 0 they go as soon as it says so ("When the agent registers", above) |
| `--dial on\|off` | Whether the car dials a phone of the agent's (above); on by default. With off, the agent takes only the links phones open, as before |
| `--dial-knock on\|off` | With the dial: on (the default), the car dials a phone when its app knocks, and otherwise only in the pairing window, at `nbtctl serial dial` and for the first 2 minutes of a trial of the agent, with no ask and no poll, and lists `serial-knock`; off, it asks and polls as before the knock, with the rule that leaves some phones' own links to another program, for comparisons. Not with `--dial off` |
| `--dial-poll <f>,<l>,<t>` | With `--dial-knock off` only: the dial's polls, every f seconds (1 to 3600) for l seconds (0 to 86400) after a phone connects or a link ends, then every t seconds (1 to 3600), in place of 10,120,30 |

When Bluetooth serial has started, six lines in the log say what the agent goes by: the time and what becomes
of a link that is let go, what becomes of a link the agent ends, what it does about a registration that is not
taken or is lost, when it registers and how it checks the registration, the programs the gate holds off as they are declared at that
moment (the list is taken anew each time the hold goes on), and how the car dials a phone:

```
bluetooth: a link is the agent's if it opens with the magic bytes within 500 ms of the open of its node, after any whole frames left over from an earlier link. Any other is let go. If a program declared for a serial service is running, the gate has seen another program open the node, or a program of software put on by hand is running, what was read from it is put back into its node, which stays open, unread, until the link ends, for the program it belongs to; where that cannot be done, its node is closed for that program. Otherwise it is read until the link ends or one of those signs comes. A link that opens with the knock's magic bytes there instead is the phone's knock: nothing is written into it, the car is to dial the phone that opened it, and its node is read, with what comes dropped, until the link ends. Every link a phone opens is read so, whoever the phone is
bluetooth: a link the agent ends keeps its node, read and with what comes dropped, until the phone ends the link; the log says so if it is still up 5000 ms after the agent's end closed
bluetooth: a serial service that the car's Bluetooth service does not take, or that is not registered for 20 s, is given up: the gate is taken away, and both are set up again 30 s later, for as long as it takes
bluetooth: no serial registration goes until the car's Bluetooth has said for 10 s that it is on, counted from the attach when it is on then, with the stand-ins for the declared serial services of other programs ahead of the agent's own; switching Bluetooth off drops them all, and the car says so. While the service is registered, no link of the agent's is up and no phone has asked for a link for 30 s, the registration is checked every 30 s by registering it again, after the stand-ins: a car whose Bluetooth says nothing of being switched off drops it all the same
bluetooth: while a link is being decided on or is the agent's, the gate also refuses opens by radiod
bluetooth: the car dials a phone on its instance 'nbtapp' (/dev/nbtapp) when the phone's app knocks, and otherwise only while the pairing window is open, at serial.dial and while this version of the agent is on trial (--dial-knock on): a link the phone opens that starts with the knock's magic bytes within the decide time is that phone's knock, and a phone's request for a serial link is no knock, whatever becomes of it. A knock of any phone that is connected to the car and paired with it, one of the agent's or not, has the car dial it at once, with no ask before it, when no link of the agent's is up and no dial is out; one that comes while a phone's request is pending, a link is being decided on or another phone's dial is out waits 4000 ms at most. After a dial a knock brought that gave the agent no link, that phone's knocks are ignored for 3000 ms. No poll, and no ask of a phone's services. While the pairing window is open every phone there is dialled every 5 s, and while this version of the agent is on trial and no link of the agent's is up, every phone of the agent's there every 5 s, for 120 s from when the trial is known, the serial service registered and the car's device list come; after that, until the version is confirmed, a phone is dialled when its app knocks, as when it is not on trial. The app's magic bytes are waited for 3000 ms; a dial with no answer is given up after 95 s; a link left behind on the instance that a dial finds is ended, and the phone dialled again at once. A dialled link that the agent ends has 5000 ms to be ended by the phone before the car is asked to, and the car's answer is waited for 3000 ms. A phone's request that the car ends while the agent's dial is out, or up to 1000 ms after it was answered, is not counted as a refusal. A registration refused while /dev/nbtapp is there: the car's device list is waited for 3000 ms and the link ended for each phone there, 15 s at most in all, and the service registered again at once if it has gone. As soon as the car answers a dial with a failure other than 10 or BUSY, one re-point request goes on the same instance, so that a link end the car may book against that instance names no phone: its answer is waited for 3000 ms, with no dial meanwhile, and an answer other than 5 within 500 ms stops these requests for the rest of the run
```

With `--dial-knock off` the first of these ends as before the knock, with the rule that leaves some phones' own
links to another program ("Other programs' links"), and the last says the polls and the asks instead.

It ignores SIGHUP, so it goes on when the SSH session ends; `kill` with its process id stops it, and so does
`nbtctl stop`, which also keeps the iDrive plugin from starting it again until the head unit powers down. Its log
is its standard error and nothing else, so the file above grows in `/tmp`, which is memory, for as long as the
agent runs.
After a power down the programs are gone from `/tmp` and the agent has to be copied and started again.

### Kept on the head unit: the install script, the starter and a version's launcher

`hu_install.sh` copies the agent to `/var/opt/nbtapp/agent`, which a power down leaves alone: the two programs
and two scripts into a folder of their version's own, and the starter beside that folder.

- **The starter**, `nbtstart.sh`, is in the agent's folder as `launcher`. It is what a person or the iDrive
  plugin starts. It decides which version is started, and belongs to no version.
- **A version's launcher**, `nbtlaunch.sh`, is in the version's folder beside `nbtagent`. The starter runs it for
  the version it has chosen, and it starts that version's agent, and starts it again when it fails.

A new version of the agent therefore comes with its own launcher, which is tried with it: a launcher that does
not run on a head unit fails its version's try, and the version before is back, with the launcher it had.

```sh
ksh /mnt/umassXXXXX/agent/hu_install.sh              # says what it did, and how to start the starter
/var/opt/nbtapp/agent/launcher --root /tmp/hu &      # the starter's arguments are nbtagent's
cat /tmp/nbtapp/log/agent.log                        # the log of this start; a power down empties /tmp
slay nbtagent                                        # SIGTERM to the agent: it ends, and its launcher and the starter with it
nbtctl stop                                          # the same, asked over its socket (agent.stop), and it leaves agent.stopped
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
them: nothing on the head unit checks a first install's signature.

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

One more file of the start is not there but on the RAM disk, so that a power down takes it away:
`/tmp/nbtapp/agent.stopped` (`DIR/run/agent.stopped` with `--root`). The agent leaves it, empty, when a controller
stopped it with `agent.stop` (**Stop the agent**, or `nbtctl stop`), and it keeps the plugin from starting the
starter when the HMI starts; the next agent that starts removes it. A signal leaves none ("Stopping from the
iDrive menu", below).

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
| 0 | The agent was stopped: SIGTERM, or `agent.stop` | Ends with 0 | Ends |
| 2 | It was started with arguments it does not take | Ends with 2 | Ends with 2 |
| 3 | A new start would change nothing: another agent has the socket, or this build does not know the head unit | Ends with 3 | Ends with 3 |
| 4 | The agent has placed a newer version of itself and asks to be started again | Ends with 4 | Looks at the files again at once, which starts that version |
| Any other, or a signal | A failure | Starts the agent again after 2 s; after five failures in a row it ends with 1 | Ends with 1 |

A version on trial gets one start. Its launcher ends with the agent's status whatever that is, and for the
starter any status but 0 and 4 is a try that failed, a launcher that did not run at all included: it copies the
start's log to `log/trial.log` and looks at the files again, which starts the version in `current`. For the
version in `current`, a starter that ends with its launcher's status other than 0 says so in the log first, as
the last line of `agent.log`: `launcher: version 1.5.1's launcher ended with status 1: the starter ends with the
same`, or, for a launcher that cannot be run at all, `... could not be run (status 126)` or `... was not found
(status 127)`.

To stop the agent, stop the agent (`slay nbtagent`, or `nbtctl stop`): its launcher and the starter follow. An
agent on trial that is stopped ends its try: the starter finds `trial.started` at its next start and runs the
version in `current`, which undoes the update. A starter or a launcher that is killed leaves the agent running
with nothing to start it again.

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
log of each start names it: `launcher: starter revision 3`, after what the starter read in the files where it
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
- `/tmp/nbtapp/agent.stopped` is there: the agent was stopped from the menu or with `nbtctl stop` since the head
  unit powered up ("Stopping from the iDrive menu", below). Nothing goes on record for that;
- three starts with the head unit are on record that no agent outlasted by five minutes.

That record is `autostart.boots`. The agent puts a gate in front of the car's Bluetooth serial node; one that
brought a head unit down would do so again at every start, with nobody at a console to stop it. So the plugin
writes each start down before it makes it, and the agent removes the file once it has run for five minutes or has
been stopped. Three starts on record, and the plugin starts nothing and says `NOT started` in the log. A start
counts for 15 minutes; with the head unit's clock not set, which is how many start, it counts until the file is
removed. To have the agent started again: `rm /var/opt/nbtapp/agent/autostart.boots`, or start the launcher by
hand, at the console or from the iDrive menu (below), and leave the agent running for five minutes.

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

#### Stopping from the iDrive menu

**NBTApp**, then **Stop the agent**. The first row says what it does, **Off until the head unit restarts**, and
the second, **Stop it now**, asks the agent to stop (`agent.stop` over its local socket, as `nbtctl stop` sends it).
The agent answers at once and ends as it does for `slay nbtagent`: the phones are told that it is stopping,
Bluetooth serial is taken away, and its launcher and the starter end with it. The screen reads **Stopping the
agent...** and keeps its link to the agent until the agent has gone, then **The agent is stopped**; if the agent
has not gone 30 seconds after its answer, **The agent did not stop**. While a job runs or a restart is on its way
the agent refuses (**A job is running. Try again later**); an agent from before `agent.stop` answers that it has
no such method (**The agent cannot do this. Update it**); with no agent running the screen says **The agent is not
running**. One press per showing; the page starts over when it is shown again. It needs no SSH.

An agent stopped this way leaves the empty file `/tmp/nbtapp/agent.stopped` on the RAM disk. While it is there the
plugin does not start the starter when the HMI starts, whatever `autostart` says, and says why in its log
(`agent: NOT started: the agent was stopped from the menu or with nbtctl ...`). What starts the agent again:

- the head unit powering down, which empties the RAM disk: at the next start of the head unit the plugin starts
  the agent as before, where `autostart` asks for it;
- **Start the agent** in the menu, which does not look at the file, or the starter started by hand at a console.
  The agent that starts removes the file, and its log says so.

`slay nbtagent`, or any other signal, leaves no file, and the plugin goes on as before. A version of the agent on
trial may be stopped too: its try ends unconfirmed, and the next start runs the version before it, which undoes
the update ("The agent updates itself", below).

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

Once a version is confirmed nothing goes back by itself. Going back by hand, by writing an older version's name to
`current`, has one limit: the agent reads each installed package's manifest again from its record, and an older
agent refuses a manifest whose form it does not know. A package installed by a newer agent with, for example, a
`logs` path in `/dev/shmem` or in a folder only that agent knows (skipped there), a file in `${var_plugins}`, or a
`requires.app`, is then a record the older agent cannot read, so it cannot list, update or remove that package
until the newer version is back.

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
- From the agent it names in `requires.agent`. The package of this repo names 1.6.0, the first version that
  knows `requires.app`, in which it names the app that goes with it. A head unit with an agent from 1.1.0, the
  first version whose folder holds a launcher, to 1.5.1 updates to 1.6.0 first, which names 1.1.0 and which the
  app offers as the newest version that runs there, and then to the newest. The versions before 1.1.0 were for
  trials, and a head unit that has one is installed anew with `hu_install.sh`.

A `job.status` from the iDrive plugin does not confirm a version: the plugin would reach an agent that no phone
can. `nbtctl status` does, as a person at the console.

The catalogue lists `org.nbtapp.agent`, and the phone app follows a job through `restarting`.

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
  runs: what it fetches, what it installs or removes, and the job's record. A phone that connects, says hello,
  asks what is installed and has the checks run causes no write; one that pairs, or does `auth` on a serial link
  from a Bluetooth address the agent did not have for it, has that address written to `phones.json`. With
  Bluetooth serial, a problem writes a line to `serial-problems.log` ("The problem file", above), the first of
  each kind in a run, 16 at most; a run with none does not touch that file. A check's fix writes what it moves and
  removes, and its lines in the install log. A channel writes nothing to flash: its switch, the pid of the
  program the agent started for it, its socket and its program's log are on the RAM disk (`/tmp/nbtapp` and the
  package's `${tmp}`), and a power down takes them.
- **A version of the agent on trial** writes none of its state until a phone has confirmed it: the job's
  record, the journal and the install log stay as the version before left them, for that version to read if it
  comes back. The confirmation then writes `current`, the package's record and the job's.
- **The log's size**: a version's launcher names the log's file to the agent (`NBTAGENT_LOG`), and the agent keeps it
  short. A file that has passed 512 KB becomes `agent.log.1`, over the one that was there, and the log goes on in
  a new file, so the three files hold 1.5 MB at most.

What an agent says is on the RAM disk and is gone after a power down; the copy above is the one exception. A
try that a power down ends leaves no log, and that is how a try ends whose agent no phone could reach. To find
out why, try the update again and read the log before the head unit is turned off.

## Checks

When an agent first comes to a head unit, there may be software on it that was put there by hand, outside this
system, and that stands in the way of the head unit's own working. The agent has a small table of checks for it,
built in (`agent/checks.hpp`). Each check is one definition: its id, its texts, a detect that reads and writes
nothing, the fix where it has one, and the restart its fix leaves needed. A controller has the checks run, shows
what they found, and asks for a check's fix once the user has said yes. An agent from 1.4.0 has them, and lists
the capability `checks` in `hello`.

| Check | What it finds | Its fix |
|---|---|---|
| `system-writable` | The system partition writable where no install step runs and the agent did not remount it: it was remounted by hand and not put back | Remounts it read-only with the configured command, and asks the file system whether it is. Nothing is listed, moved or removed. On a folder that stands in for a head unit the question does not apply, and the check finds nothing |
| `aa-fix` | An app that was installed by hand: `/opt/aa` is there, or a plugin folder whose `<Name>.manifest` claims the connector id 210 and in which no installed package owns anything. It lists such a plugin folder to be moved, and everything in `/opt/aa` and `/var/opt/aa` to be removed | Stops the programs that run from `/opt/aa`. Moves the plugin's folder, whole, out of the plugin folder into `/opt/aa`, or into `/var/opt/aa` from `${var_plugins}`, as `<Name>.old`, or `<Name>.nbtappsold` where that is taken, and leaves it there, where the HMI no longer loads it. Then removes the rest as listed. The packet filter's configuration is not read or written: what that app changed there stays as it is. The HMI needs a restart |

The plugin's folder is moved and not removed because the HMI may have the plugin loaded, and its files are not
taken away under it. The check looks for it in every folder the HMI loads plugins from, `${plugins}` and
`${var_plugins}` both. On a car the second is on another partition than `/opt/aa`, and a move is one rename
(below), so a plugin found there is moved into `/var/opt/aa`, the folder of the app's data, which is on the
writable partition too; a plugin found anywhere else goes into `/opt/aa`. A folder directly in `/opt/aa` or
`/var/opt/aa` that is a real folder, not a link, and whose name ends in `.old` or `.nbtappsold` is parked: neither
it nor anything in it is listed, moved or removed, and a check that finds nothing else says `ok`. Neither folder
is listed for removal itself while it receives a plugin or holds a parked one.

Every fix keeps to these rules:

- It does what the user was shown and nothing else. A problem comes with a token, SHA-256 over the check's id, its
  sorted list of paths with their kinds, and each move with its two ends; the controller sends it back with
  `checks.fix`. The fix lists again, and refuses with `stale` if the token is not that of what the check finds now.
  A file that came after the list stays, and so does its folder.
- A move is one rename, to the destination the user was shown, and only within one file system: it is never a copy
  and a removal. A move onto something that is there is not made, and the folder is reported as left. Where both
  names are taken, where the plugin's folder is on another file system than the folder it goes into (`/opt/aa`, or
  `/var/opt/aa` for one from `${var_plugins}`), or where that folder is a link or a file, there is no fix. The
  moves come before any removal.
- It removes one path at a time: a file or a link is unlinked by its full path, and a folder is removed only when
  it is empty, the deepest first. Nothing is removed by a wildcard or with what it holds, and nothing is walked at
  the time of the removal.
- Nothing is followed through a link. A link is listed as a link and unlinked as one, and what it leads to stays;
  a folder that is a link is not entered. A device, a socket or a fifo is never removed, and is reported as left.
- The listing stays on the file system its root is on, goes four levels below a root at most, and holds 256 entries
  at most; a check offers to move 8 plugin folders at most. Past a bound, on another file system, or with a name
  that cannot be shown as text, the check is a problem with no fix, and its detail says why.
- A path that an installed package's record owns is never listed, and neither is anything in the agent's own
  folders: its state, staging and run folders, the agent's folder, and the folders of the packages' `${app}`,
  `${data}` and `${tmp}`.
- Programs come first. A program whose executable is in the check's folder is sent SIGTERM, and SIGKILL after 5
  seconds; one that is still there a second later, or processes that cannot be read, end the fix before anything
  is moved or removed. The restart mark is raised before the first change, the system partition is writable for
  the moves and the removals only, and its folders go before it is put back read-only.
- A fix that is cut off, by a power cut or a kill, is run again: the check lists what is left, with a new token,
  and the fix goes on from there. A plugin that was moved is parked, and is not listed again. There is no journal.
- Each move and each path a fix removes is a line in the install log, `install.log` in the state folder, and the
  agent's log has a line for each fix: how many folders were moved, how many paths went, how many are left, and the
  programs it stopped.

The two messages, in full in `agent/agent.hpp`:

```
checks.run { "id"?, "after"? }   -> { "checks": [ { "id", "state": "ok" | "problem" | "unknown", ... } ] }
checks.fix { "id", "token" }     -> { "state", "removed", "moved", "left", "leftMore", "stopped", "restart" }
```

A problem has `title`, `detail`, `count`, the paths it would remove in `found`, `more`, the folders it would move
in `moves` (each with `from` and `to`), and `fix`, with its `label`, `detail`, `restart` and `token`, where there is
one; `unknown` has `why`. A problem may have moves and no paths. With no `id` every check is answered, each with as
many of its paths as the link's frame takes, and `more` where there are others: the controller asks for those with
`id` and `after`, the last path it has, and gets the same token while nothing has changed. The moves are on every
page whole. A fix answers the state the check finds afterwards, how many paths went, the moves it made, the paths
that are left and the folders that were not moved, as far as the frame takes and how many more, and the restart
that is needed. Both are refused before `auth`, like every other method. `checks.run` writes nothing, so it is
answered on an agent on trial too, and refused with `busy` only while an install step runs. `checks.fix` is
refused with `busy` while a job runs and on an agent on trial, with `forbidden` to the iDrive plugin, and with
`not-found`, `stale`, `unsupported` (the check has no fix for what it finds) and `io-error` (what could not be read,
or the program that did not end) as their names say.

At the console:

```sh
nbtctl checks                     # a line for each check: its id and state, and for a problem the number of paths
nbtctl checks --list              # and each folder it would move, and every one of those paths
nbtctl checks fix aa-fix          # the check, then its fix: what was moved, what went, what is left, what was stopped
```

`nbtctl checks fix` exits with 1 while the check still finds a problem. `hu_install.sh` says in one line, at its
end, when `/opt/aa` is there.

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
  `pairing.close`, `phones.forget`, `serial.reset` and `agent.stop`, and what they answer;
- the files a plugin goes by when it starts the agent: `launcher`, `autostart` and `autostart.boots` in
  `/var/opt/nbtapp/agent`, `/tmp/nbtapp/log/launcher.pid`, `/tmp/nbtapp/agent.stopped`, and
  `/var/opt/nbtapp/disable`. A plugin starts `launcher` with `/bin/ksh` and `NBT_LAUNCH_DELAY`, so a starter of
  any version takes both.

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
the new version, and then the plugin's, which restarts the HMI.

## License

PolyForm Strict License 1.0.0, as the whole repo: see [LICENSE.md](../LICENSE.md) at its root.

`agent/vendor/tweetnacl/` is third-party code in the public domain; see the README in that folder.
`ssh/dropbear` and `ssh/openssh` are third-party code under their own licences, and `ssh/dropbear-qnx.patch` and
`ssh/localoptions.h` are MIT, as Dropbear; see `ssh/README`.

Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)

## Disclaimer

This loads code into the head unit's HMI process. A fault there can stop iDrive from starting. The code comes as is;
you are responsible for what you do to your own car.

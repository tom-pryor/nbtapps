# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#
# NBTApp: a native iDrive PluginApp for BMW NBT Evo (QNX Neutrino 6.5.0 SP1, ARMv7LE, Cortex-A15).
# ./build.sh runs the right target in the right place; by hand:
#
#   make lib        release/NbtApp/libNbtApp.so     needs the QNX SDP 6.5.0 (qcc, GCC 4.4.2). Its screens about
#                   phones and its start of the agent with the head unit are tested on the host, with the
#                   agent's tests (tests/agent/test_plugin.cpp, test_agent_start.cpp)
#   make pkg        manifest, XML, text / image DBs needs python3 (host)
#   make            lib + pkg: release/NbtApp/ = the folder to copy to /opt/hmi/ID5/data/ro/Plugins/
#   make plugin-pkg release/pkg/org.nbtapp.plugin-<version>.nbtpkg: the plugin's own package, made of release/NbtApp/,
#                   not signed   needs python3 (host)
#   make plugin-release KEY=FILE   signs that package for the catalogue, to release/pkg/signed/, as agent-release
#                   does the agent's   needs python3 + cryptography, git (host)
#   make check      ids, XML, the built library and the plugin's package  needs python3 + readelf (host)
#   make agent-test tests of the agent (agent/): package reader, install engine, protocol, nbtagent with
#                   nbtctl end to end, and the launcher and install scripts. The agent's side runs in a Docker
#                   container (tools/bench.py), so that the packages' scripts do not run loose on this machine
#                   needs docker, g++, gcc, zlib, python3 + cryptography (host). About a minute, more the first time
#   make bench-image the Docker image for that: nbtagent and the test programs, built inside it
#   make agent-host build/agent-host/: nbtagent and nbtctl for this machine. nbtctl is the one to use; run the
#                   agent with tools/bench.py
#   make agent      the same code, tests and programs compiled for the head unit, and release/agent/: nbtagent,
#                   nbtctl, their version and the scripts hu_probe.sh, hu_install.sh, nbtlaunch.sh and nbtstart.sh,
#                   the folder to take to a head unit   needs the SDP
#   make agent-pkg  release/pkg/org.nbtapp.agent-<version>.nbtpkg: the agent's own package, made of release/agent/'s
#                   programs, the version's launcher and starter (agent/'s nbtlaunch.sh and nbtstart.sh) and what
#                   a first install needs beside them (agent/'s hu_install.sh, and the version's file), not signed
#                   needs python3 (host)
#   make agent-check release/agent/ looked at: the head unit's kind of program, nothing of the test bench in it,
#                   the official package key in nbtagent; and release/pkg/: the package is those programs
#                   needs python3 + readelf (host)
#   make agent-release KEY=FILE   signs that package for the catalogue, to release/pkg/signed/. KEY is the private
#                   half of the package key. Refuses programs that are not built from the commit checked out
#                   needs python3 + cryptography, git (host)
#   make test-agent-pkg   host test of tools/agent_pkg.py, which those two run   needs python3 + cryptography
#   make test-plugin-pkg  host test of tools/plugin_pkg.py   needs python3 + cryptography
#   make test-pkg   host test of tools/nbtpkg.py and the test packages   needs python3 + cryptography
#   make testdata   build/testdata/: good and deliberately broken .nbtpkg files for every reader's tests
#   make icons      redraw assets/*.png             needs python3 + Pillow (host)
#   make clean

QCC ?= qcc

# Goes into the library's log lines. build.sh passes it, because the SDP image has no git.
VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo unknown)

# GNU libstdc++ (_gpp variant) to match HmiMain3D, PIC shared object, soft-float call ABI.
CXX      = $(QCC) -Vgcc_ntoarmv7le_gpp
CXXFLAGS = -O2 -Wall -D_QNX_SOURCE -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp -fPIC -I./src \
           -DNBT_APP_VERSION='"$(VERSION)"'

BUILD = build
OUT   = release/NbtApp

# The HU Framework (libAppFramework.A.so, libCoreFramework.A.so) is only needed at link time, for its sonames and
# symbol names.
# By default they are empty stubs built from stubs/*.syms. To link against a head unit's real ones instead:
#   make lib FRAMEWORK_LIBDIR=/path/to/opt/hmi/ID5/lib
FRAMEWORK_LIBDIR ?= $(BUILD)/stubs
FRAMEWORK_LIBS = $(FRAMEWORK_LIBDIR)/libAppFramework.A.so $(FRAMEWORK_LIBDIR)/libCoreFramework.A.so

SRCS = src/nbt_app.cpp src/crc32.cpp
# What the library takes from the agent: the frames, the JSON and the messages of the protocol, which it speaks to
# the agent over the agent's local socket (src/agent_link.hpp). Not the agent's CRC32, which is zlib's: src/crc32.cpp.
PLUGIN_AGENT_SRCS = agent/frame.cpp agent/json.cpp agent/peer.cpp
HDRS = $(wildcard src/*.hpp) $(wildcard agent/*.hpp)

.PHONY: all lib pkg check agent agent-check agent-test agent-host agent-programs agent-e2e agent-launcher \
        agent-pkg agent-release test-agent-pkg plugin-pkg plugin-release test-plugin-pkg \
        bench-image \
        test-protocol test-pkg testdata icons clean FORCE
.SECONDARY:

all: lib pkg
	@echo [DONE] $(OUT): copy this folder to /opt/hmi/ID5/data/ro/Plugins/NbtApp

lib: $(OUT)/libNbtApp.so

# -z defs: a declaration whose mangled name the HU Framework does not export fails here, not on the car.
# --allow-shlib-undefined: the real HU Framework libraries have dependencies of their own that are not at hand.
# -lsocket: the link to the agent is a local socket.
$(OUT)/libNbtApp.so: $(SRCS) $(PLUGIN_AGENT_SRCS) $(HDRS) $(FRAMEWORK_LIBS) $(BUILD)/version
	@echo [BUILD] $@ $(VERSION)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -shared -Wl,-soname,libNbtApp.so -o $@ $(SRCS) $(PLUGIN_AGENT_SRCS) $(FRAMEWORK_LIBS) \
	    -Wl,-z,defs -Wl,--allow-shlib-undefined -lsocket -lang-c++

# Rewritten only when VERSION changes, so a new version relinks the library and nothing else does.
$(BUILD)/version: FORCE
	@mkdir -p $(BUILD)
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@

# ---- link stubs: one empty function or object per listed symbol, under the real library's soname ----------------
$(BUILD)/stubs/%.s: stubs/%.syms
	@mkdir -p $(BUILD)/stubs
	awk '$$1 == "F" { printf "\t.text\n\t.globl %s\n\t.type %s, %%function\n%s:\n\tbx lr\n", $$2, $$2, $$2 } \
	     $$1 == "O" { printf "\t.data\n\t.globl %s\n\t.type %s, %%object\n\t.size %s, 4\n%s:\n\t.word 0\n", $$2, $$2, $$2, $$2 }' \
	    $< > $@

$(BUILD)/stubs/%.so: $(BUILD)/stubs/%.s
	@echo [STUB] $@
	$(QCC) -Vgcc_ntoarmv7le -shared -nostdlib -Wl,-soname,$*.so -o $@ $<

# ---- QNX executables -------------------------------------------------------------------------------------------
# Plain executables: Dinkumware C++ library (_cpp variant), no -fPIC (a PIC executable does not start on the HU).
TOOL_CXX      = $(QCC) -Vgcc_ntoarmv7le_cpp
TOOL_CXXFLAGS = -O2 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp

# ---- head-unit agent (agent/): package reader, install engine, protocol, and the two programs --------------------
#   sha256    SHA-256 and HMAC-SHA256           json      the strict JSON profile of manifests and messages
#   inflate   raw inflate and CRC32, on zlib    ed25519   signature verification, on the vendored TweetNaCl
#   semver    versions and version ranges       manifest  manifest.json, checked as spec/manifest.schema.json says
#   package   the .nbtpkg reader                fsutil    file-system steps: whole files, moves, removal
#   process   runs a hook script                journal   the write-ahead journal of the step in progress
#   store     installed records, on disk        hmi_ids   the ids of an HMI plugin that must be unique
#   engine    install, upgrade, remove, undo, and recovery after a power cut
#   frame     the protocol's frames             link      reliable bytes, over a socket
#   peer      magic bytes, frames, messages     planner   job.plan: the steps of a job, and what speaks against it
#   protocol_constants  what spec/protocol/constants.txt holds: the names and numbers a phone app has to share
#   job       fetch, install, events, records   agent     every method, and the loop that serves the links
#   hu_platform  the head unit's platform: remounts and restarts by configured commands
#   dsi_client   the head unit's service-to-service IPC, through its own library
#   spp_service  our Bluetooth serial service: its registration with the Bluetooth stack
#   spp_gate     the gate in front of /dev/btspp0, which keeps the car's media process off a serial link
#   spp_nodes    which serial node holds a link of ours
#   serial_link  the Bluetooth serial listener, and a link on a serial node
#   programs     which programs are running, by the file names of their executables
#   serial_services  the serial services that packages' programs register, and the removal of one whose program
#                    is not running
#   phones       the paired phones and their pairing keys, on disk
#   agent_home   the agent's own folder: its versions, and the files that say which one the launcher starts
#   log_files    the files a controller may pull: the logs a package's manifest names, and the agent's own
# One static library, one test program per tests/agent/test_*.cpp, and two programs: nbtagent (the agent: on a
# folder that stands in for the head unit, and in the build for the head unit also on the head unit itself) and
# nbtctl (a command-line controller, and a package source from a folder). `make agent-test` builds all of it into a Docker image and runs it there: the engine runs packages'
# hook scripts, and a script is not held to the folder the agent works in, so the agent's side of every test is
# kept in a container (tools/bench.py; nbtctl, which runs nothing, is this machine's own). `make agent` builds the
# same for the head unit, where it cannot run here: it shows that the code compiles and links with qcc. Nothing
# here has run on a car. What the head unit's nbtagent goes by when it works on one is HeadUnitConfig::car(), in
# agent/hu_platform.cpp.
AGENT_SRCS   = agent/sha256.cpp agent/json.cpp agent/inflate.cpp agent/ed25519.cpp agent/semver.cpp \
               agent/manifest.cpp agent/package.cpp agent/fsutil.cpp agent/process.cpp agent/journal.cpp \
               agent/store.cpp agent/hmi_ids.cpp agent/engine.cpp agent/frame.cpp agent/link.cpp agent/peer.cpp \
               agent/planner.cpp agent/job.cpp agent/agent.cpp agent/hu_platform.cpp agent/dsi_client.cpp \
               agent/spp_service.cpp agent/spp_gate.cpp agent/serial_link.cpp agent/programs.cpp \
               agent/serial_services.cpp agent/phones.cpp agent/agent_home.cpp agent/log_files.cpp
AGENT_C_SRCS = agent/vendor/tweetnacl/tweetnacl.c
AGENT_HDRS   = $(wildcard agent/*.hpp) $(wildcard agent/vendor/*/*.h)
# src/*.hpp: test_plugin runs the plugin's screens about phones, which are headers, against an agent, and
# test_agent_start the plugin's start of the launcher, with agent/nbtstart.sh as the starter it starts and
# agent/nbtlaunch.sh as the launcher of the version that starter chooses.
AGENT_TEST_HDRS = $(wildcard tests/agent/*.hpp) $(wildcard src/*.hpp)
AGENT_TESTS  = $(patsubst tests/agent/%.cpp,%,$(wildcard tests/agent/test_*.cpp))
AGENT_PROGS  = nbtagent nbtctl
agent_objs   = $(patsubst agent/%.cpp,$(1)/%.o,$(AGENT_SRCS)) $(patsubst agent/vendor/tweetnacl/%.c,$(1)/%.o,$(AGENT_C_SRCS))

HOST_CXX ?= g++
HOST_CC  ?= gcc
AGENT_HOST = $(BUILD)/agent-host
AGENT_HOST_CXXFLAGS = -std=gnu++0x -O1 -g -Wall -Wextra -DNBT_HOST_TEST
# Defined before the rules that name it as a prerequisite: make reads a rule's prerequisites as it meets them.
TESTDATA = $(BUILD)/testdata/vectors.json

agent-test: $(addprefix run-agent-,$(AGENT_TESTS)) test-protocol test-agent-pkg test-plugin-pkg agent-e2e agent-launcher
	@echo [DONE] $(words $(AGENT_TESTS)) agent tests, the protocol vectors, the tests of the two package scripts, the end-to-end test and the launcher test passed

agent-host: $(addprefix $(AGENT_HOST)/,$(AGENT_PROGS))
	@echo [DONE] $(AGENT_HOST): nbtagent and nbtctl for this machine

# Everything the bench image holds. tools/bench/Dockerfile runs this inside the image.
agent-programs: $(addprefix $(AGENT_HOST)/,$(AGENT_TESTS) $(AGENT_PROGS) nbtagent-next)

# Asked for on every run, because only Docker can tell whether the image is this checkout's; it is quick to say so.
bench-image:
	@python3 tools/bench.py image

# nbtagent, in a container, and nbtctl as two processes: plan, fetch, install, upgrade, a cancelled and a dropped
# fetch, removal.
agent-e2e: bench-image $(AGENT_HOST)/nbtctl $(TESTDATA)
	@echo [TEST] agent end to end
	@python3 tests/e2e/run_agent.py $(AGENT_HOST) $(BUILD)/testdata

# The starter (agent/nbtstart.sh), a version's launcher (agent/nbtlaunch.sh) and the first install
# (agent/hu_install.sh), in a container: on folders, with stand-ins for nbtagent that end as each case says, with
# nbtagent itself ended by a signal, and with nbtagent updating itself to its second build through its own
# package, which tools/agent_pkg.py makes of that build as it makes the one for a head unit.
agent-launcher: bench-image
	@echo [TEST] agent launcher and install script
	@python3 tests/e2e/run_launcher.py

# spec/protocol/*.vectors read by a second implementation, so that a fault in the agent's codec is not also the
# measure it is checked against.
test-protocol:
	@echo [TEST] spec/protocol
	@python3 tests/test_protocol_vectors.py

$(AGENT_HOST)/%.o: agent/%.cpp $(AGENT_HDRS)
	@mkdir -p $(AGENT_HOST)
	$(HOST_CXX) $(AGENT_HOST_CXXFLAGS) -c -o $@ $<

# Third-party C, compiled as it comes: no warning flags, because it is not ours to tidy.
$(AGENT_HOST)/%.o: agent/vendor/tweetnacl/%.c $(AGENT_HDRS)
	@mkdir -p $(AGENT_HOST)
	$(HOST_CC) -O2 -c -o $@ $<

# The Makefile is a prerequisite because `.SECONDARY:` above keeps make from noticing an object that does not exist
# yet: a source added to the lists would otherwise stay out of a library that is already built.
$(AGENT_HOST)/libnbtagent.a: $(call agent_objs,$(AGENT_HOST)) Makefile
	@rm -f $@
	ar rcs $@ $(filter %.o,$^)

$(AGENT_HOST)/test_%: tests/agent/test_%.cpp $(AGENT_HOST)/libnbtagent.a $(AGENT_HDRS) $(AGENT_TEST_HDRS)
	$(HOST_CXX) $(AGENT_HOST_CXXFLAGS) -o $@ $< $(AGENT_HOST)/libnbtagent.a -lz

# NBT_TEST_KEYS gives the host's nbtagent its --test-key option and NBT_DEBUG_TCP its --debug-tcp option (a TCP
# link on the loopback address, over which a phone is paired in the clear). The head unit's build below has
# neither, and nbtagent.cpp does not compile for it with either.
$(AGENT_HOST)/nbtagent: agent/nbtagent.cpp $(AGENT_HOST)/libnbtagent.a $(AGENT_HDRS) $(BUILD)/version
	$(HOST_CXX) $(AGENT_HOST_CXXFLAGS) -DNBT_TEST_KEYS -DNBT_DEBUG_TCP -DNBT_APP_VERSION='"$(VERSION)"' -o $@ $< $(AGENT_HOST)/libnbtagent.a -lz

$(AGENT_HOST)/nbtctl: agent/nbtctl.cpp $(AGENT_HOST)/libnbtagent.a $(AGENT_HDRS)
	$(HOST_CXX) $(AGENT_HOST_CXXFLAGS) -o $@ $< $(AGENT_HOST)/libnbtagent.a -lz

# The same nbtagent, built to say that it is a later version (agent/version.hpp), so that an update of the agent
# by the agent can be run from end to end: tests/e2e/run_launcher.py makes a package of this program, as the
# version it says it is. Test bench only, like the options above.
AGENT_NEXT_VERSION = 1.3.0
$(AGENT_HOST)/nbtagent-next: agent/nbtagent.cpp $(AGENT_HOST)/libnbtagent.a $(AGENT_HDRS) $(BUILD)/version
	$(HOST_CXX) $(AGENT_HOST_CXXFLAGS) -DNBT_TEST_KEYS -DNBT_DEBUG_TCP -DNBT_TEST_AGENT_VERSION='"$(AGENT_NEXT_VERSION)"' \
	    -DNBT_APP_VERSION='"$(VERSION)"' -o $@ $< $(AGENT_HOST)/libnbtagent.a -lz

# Each test program runs in a container that may read this folder and write build/bench/work, which is where the
# ones that keep files are told to keep them.
BENCH_WORK = $(BUILD)/bench/work
run-agent-test_fsutil: TEST_ARGS = $(BENCH_WORK)
run-agent-test_log_cap: TEST_ARGS = $(BENCH_WORK)
run-agent-test_log_files: TEST_ARGS = $(BENCH_WORK)
run-agent-test_phones: TEST_ARGS = $(BENCH_WORK)
run-agent-test_plugin: TEST_ARGS = $(BENCH_WORK)
run-agent-test_agent_home: TEST_ARGS = $(BENCH_WORK)
run-agent-test_agent_start: TEST_ARGS = $(BENCH_WORK)
run-agent-test_engine: TEST_ARGS = $(BUILD)/testdata $(BENCH_WORK)
run-agent-test_agent:  TEST_ARGS = $(BUILD)/testdata $(BENCH_WORK)/agent
run-agent-test_hu_platform: TEST_ARGS = $(BUILD)/testdata $(BENCH_WORK)/hu
run-agent-test_serial: TEST_ARGS = $(BENCH_WORK)/serial
run-agent-test_programs: TEST_ARGS = $(BENCH_WORK)/programs
run-agent-test_serial_services: TEST_ARGS = $(BUILD)/testdata $(BENCH_WORK)

run-agent-%: bench-image
	@echo [TEST] agent/$*
	@python3 tools/bench.py test $* $(TEST_ARGS)

# The package reader and the engine are tested against the packages the Python tool builds.
run-agent-test_package run-agent-test_engine run-agent-test_agent run-agent-test_hu_platform \
run-agent-test_serial_services run-agent-test_embedded_keys: $(TESTDATA)

$(TESTDATA): tests/make_packages.py tools/nbtpkg.py tools/plugin_pkg.py tools/agent_pkg.py spec/manifest.schema.json \
             plugin/NbtApp.manifest plugin/NbtApp.xml plugin/package/manifest.json.in \
             $(shell find tests/packages tests/keys -type f 2>/dev/null)
	python3 tests/make_packages.py $(BUILD)/testdata

# For the head unit: plain executables like the Bluetooth tools (Dinkumware C++ library, no -fPIC). zlib is linked
# in from the SDP's libz.a, so nothing depends on a libz on the car.
AGENT_QNX = $(BUILD)/agent
AGENT_CC  = $(QCC) -Vgcc_ntoarmv7le
AGENT_CFLAGS = -O2 -D_QNX_SOURCE -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp

# release/agent/ is what goes to a head unit: the two programs, the file that names their version, and the
# scripts: the probe, the first install, the launcher the install puts beside the programs, and the starter it
# puts into the agent's folder as `launcher`. The test programs stay in build/agent/.
AGENT_OUT = release/agent
AGENT_SCRIPTS = hu_probe.sh hu_install.sh nbtlaunch.sh nbtstart.sh

agent: $(AGENT_QNX)/libnbtagent.a $(addprefix $(AGENT_QNX)/,$(AGENT_TESTS) $(AGENT_PROGS)) \
       $(addprefix $(AGENT_OUT)/,$(AGENT_PROGS) $(AGENT_SCRIPTS) version)
	@echo [DONE] $(AGENT_QNX): the agent library, $(words $(AGENT_TESTS)) test programs, nbtagent and nbtctl compile and link for the head unit
	@echo [DONE] $(AGENT_OUT): nbtagent, nbtctl, version and $(AGENT_SCRIPTS). make agent-check looks at them

$(addprefix $(AGENT_OUT)/,$(AGENT_SCRIPTS)): $(AGENT_OUT)/%: agent/%
	@mkdir -p $(AGENT_OUT)
	cp $< $@

# The agent's version on one line, as agent/version.hpp has it: hu_install.sh names the programs' folder by it.
$(AGENT_OUT)/version: agent/version.hpp
	@mkdir -p $(AGENT_OUT)
	sed -n 's/^const char kAgentVersion\[\] = "\(.*\)";$$/\1/p' $< > $@
	@test -s $@

$(AGENT_OUT)/%: $(AGENT_QNX)/%
	@mkdir -p $(AGENT_OUT)
	cp $< $@

# On the host: the SDP image has no python3.
agent-check:
	python3 tools/check_agent.py

# The agent's own package, org.nbtapp.agent, of the programs in release/agent/: what an agent on a head unit
# updates itself with, and, unpacked onto a USB stick, what hu_install.sh puts an agent there with for the first
# time (the package holds that script, the launcher, the starter and the version's file too). agent-pkg leaves it not signed;
# agent-release signs it with the package key, whose private half is not in this repo, and the signed archive is
# what a release carries and the catalogue records.
AGENT_PKG_OUT = release/pkg

agent-pkg:
	python3 tools/agent_pkg.py build $(AGENT_OUT) -o $(AGENT_PKG_OUT)

agent-release:
	@test -n "$(KEY)" || { echo "usage: make agent-release KEY=FILE   (the private half of the package key)" >&2; exit 2; }
	python3 tools/agent_pkg.py build $(AGENT_OUT) -o $(AGENT_PKG_OUT)
	python3 tools/check_agent.py
	python3 tools/agent_pkg.py release $(AGENT_OUT) --pkg $(AGENT_PKG_OUT) --key "$(KEY)"

$(AGENT_QNX)/%.o: agent/%.cpp $(AGENT_HDRS)
	@mkdir -p $(AGENT_QNX)
	$(TOOL_CXX) $(TOOL_CXXFLAGS) -Wextra -c -o $@ $<

$(AGENT_QNX)/%.o: agent/vendor/tweetnacl/%.c $(AGENT_HDRS)
	@mkdir -p $(AGENT_QNX)
	$(AGENT_CC) $(AGENT_CFLAGS) -c -o $@ $<

$(AGENT_QNX)/libnbtagent.a: $(call agent_objs,$(AGENT_QNX)) Makefile
	@rm -f $@
	$(QCC) -Vgcc_ntoarmv7le -A $@ $(filter %.o,$^)

AGENT_QNX_LIBS = $(AGENT_QNX)/libnbtagent.a -Wl,-Bstatic -lz -Wl,-Bdynamic -lsocket -lang-c++

$(AGENT_QNX)/test_%: tests/agent/test_%.cpp $(AGENT_QNX)/libnbtagent.a $(AGENT_HDRS) $(AGENT_TEST_HDRS)
	@echo [BUILD] $@
	$(TOOL_CXX) $(TOOL_CXXFLAGS) -o $@ $< $(AGENT_QNX_LIBS)

$(AGENT_QNX)/nbtagent: agent/nbtagent.cpp $(AGENT_QNX)/libnbtagent.a $(AGENT_HDRS) $(BUILD)/version
	@echo [BUILD] $@ $(VERSION)
	$(TOOL_CXX) $(TOOL_CXXFLAGS) -Wextra -DNBT_APP_VERSION='"$(VERSION)"' -o $@ $< $(AGENT_QNX_LIBS)

$(AGENT_QNX)/nbtctl: agent/nbtctl.cpp $(AGENT_QNX)/libnbtagent.a $(AGENT_HDRS)
	@echo [BUILD] $@
	$(TOOL_CXX) $(TOOL_CXXFLAGS) -Wextra -o $@ $< $(AGENT_QNX_LIBS)

# ---- packages ---------------------------------------------------------------------------------------------------
test-pkg:
	python3 tests/test_nbtpkg.py

test-agent-pkg:
	@echo [TEST] tools/agent_pkg.py
	@python3 tests/test_agent_pkg.py

# The iDrive plugin's own package, org.nbtapp.plugin, of the folder release/NbtApp/: what an agent installs and
# updates the plugin with. plugin-pkg leaves it not signed; plugin-release signs it with the package key, as
# agent-release signs the agent's, and the signed archive is what the catalogue records.
plugin-pkg:
	python3 tools/plugin_pkg.py build $(OUT) -o $(AGENT_PKG_OUT)

plugin-release:
	@test -n "$(KEY)" || { echo "usage: make plugin-release KEY=FILE   (the private half of the package key)" >&2; exit 2; }
	python3 tools/plugin_pkg.py build $(OUT) -o $(AGENT_PKG_OUT)
	python3 tools/check.py
	python3 tools/plugin_pkg.py release $(OUT) --pkg $(AGENT_PKG_OUT) --key "$(KEY)"

test-plugin-pkg:
	@echo [TEST] tools/plugin_pkg.py
	@python3 tests/test_plugin_pkg.py

testdata:
	python3 tests/make_packages.py $(BUILD)/testdata

# ---- host side -------------------------------------------------------------------------------------------------
pkg:
	@mkdir -p $(OUT)
	cp plugin/NbtApp.manifest plugin/NbtApp.xml $(OUT)/
	python3 tools/make_db.py $(OUT)

check:
	python3 tools/check.py

icons:
	python3 tools/make_icons.py

clean:
	rm -rf $(BUILD) release

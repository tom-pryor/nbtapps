# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# The image tools/bench.py runs the agent in: nbtagent, nbtctl and the agent's test programs, built from this
# checkout for the image's own C library, on a Debian with a shell for the hooks and no compiler. Also
# nbtagent-next, the same agent built to say it is a later version, for the tests of the agent's own update. And
# the programs of the SSH package built for the image, in /opt/nbtagent/ssh/: Dropbear's dropbearmulti and
# OpenSSH's sftp-server from the exported and patched sources in build/ssh/src/, which `make ssh-src` makes outside
# (the image has no git; tools/bench.py image runs it first), and sshrelay without its test hooks, as the head
# unit's is built; with Debian's OpenSSH client (ssh, scp, sftp, ssh-keygen), for the SSH package's end-to-end test.
#
#   tools/bench.py image        (make ssh-src, then docker build -f tools/bench/Dockerfile -t nbtapp-bench .)
#
# A second image, built from the stage `vectors` below, makes the test packages (tests/make_packages.py) for the
# app's copies and the check that they are current: tools/bench.py testdata.
#
# The Debian image is pinned by its digest. The test packages are compared byte for byte, and their deflate data is
# whatever this image's zlib makes of them: another Debian, or this machine's own zlib, can make other bytes of the
# same files. A new digest here means running android/tools/sync_agent_vectors.sh again.
#
# Nothing is downloaded while the image is built but Debian's packages. The stages are in the order of what changes
# least: Dropbear and sftp-server are built from the export and build-host.sh alone, so a change to the agent or
# the relay does not build them again.
#
# What goes into the build is listed in Dockerfile.dockerignore next to this file.
FROM debian:trixie-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258 AS toolchain
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src

# Dropbear (server, key tool, scp) and sftp-server, as `make ssh-host` builds them for this machine, from the same
# sources and with the same options; then the relay.
FROM toolchain AS ssh
COPY ssh/build-host.sh ssh/
COPY build/ssh/src build/ssh/src
RUN sh ssh/build-host.sh build/ssh/src/dropbear build/ssh/src/openssh build/ssh/bench \
    && mkdir /out \
    && cp build/ssh/bench/dropbearmulti build/ssh/bench/sftp-server /out/
COPY Makefile ./
COPY agent agent
COPY ssh/relay ssh/relay
# The image has no git, so the version comes from outside, as in build.sh.
ARG VERSION=unknown
RUN make build/ssh/bench/sshrelay VERSION="$VERSION" \
    && strip -o /out/sshrelay build/ssh/bench/sshrelay

FROM toolchain AS build
COPY Makefile ./
COPY agent agent
COPY tests/agent tests/agent
# The plugin's headers that need no HU Framework: test_plugin runs them against the agent.
COPY src src
# Everything but the two programs that carry the version is built before the version is declared. The version
# changes with every commit and every edit anywhere in the checkout, the app's too, and a new value makes Docker run
# every step after its declaration again. So a change outside what goes into the build here compiles only nbtagent
# and nbtagent-next again, the only programs here that depend on the Makefile's build/version, and the library and
# the test programs come from Docker's cache.
RUN make -j"$(nproc)" agent-programs -o build/agent-host/nbtagent -o build/agent-host/nbtagent-next \
    && mkdir /out \
    && cp build/agent-host/nbtctl build/agent-host/test_* build/agent-host/chan_* /out/
ARG VERSION=unknown
RUN make -j"$(nproc)" agent-programs VERSION="$VERSION" \
    && mkdir /out-versioned \
    && cp build/agent-host/nbtagent build/agent-host/nbtagent-next /out-versioned/

# Python and the library nbtpkg signs with, for tests/make_packages.py, and nothing of the agent: built only as
# its own image (tools/bench.py testdata), so the agent's image below stays without Python, as a head unit is.
FROM debian:trixie-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258 AS vectors
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 python3-cryptography \
    && rm -rf /var/lib/apt/lists/*

# The agent's image: the last stage, so the one a plain `docker build` makes.
FROM debian:trixie-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258
RUN apt-get update \
    && apt-get install -y --no-install-recommends openssh-client \
    && rm -rf /var/lib/apt/lists/*
COPY --from=ssh /out/ /opt/nbtagent/ssh/
COPY --from=build /out/ /opt/nbtagent/
COPY --from=build /out-versioned/ /opt/nbtagent/
# Fails the build here, and not a test later, if the image lacks a library the programs need.
RUN /opt/nbtagent/nbtagent --version && /opt/nbtagent/nbtagent-next --version && /opt/nbtagent/test_sha256 \
    && /opt/nbtagent/ssh/sshrelay --version && /opt/nbtagent/ssh/dropbearmulti dropbear -V \
    && /opt/nbtagent/ssh/sftp-server < /dev/null && ssh -V

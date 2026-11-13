# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# The image tools/bench.py runs the agent in: nbtagent, nbtctl and the agent's test programs, built from this
# checkout for the image's own C library, on a Debian with a shell for the hooks and no compiler. Also
# nbtagent-next, the same agent built to say it is a later version, for the tests of the agent's own update.
#
#   tools/bench.py image        (docker build -f tools/bench/Dockerfile -t nbtapp-bench .)
#
# A second image, built from the stage `vectors` below, makes the test packages (tests/make_packages.py) for the
# app's copies and the check that they are current: tools/bench.py testdata.
#
# The Debian image is pinned by its digest. The test packages are compared byte for byte, and their deflate data is
# whatever this image's zlib makes of them: another Debian, or this machine's own zlib, can make other bytes of the
# same files. A new digest here means running android/tools/sync_agent_vectors.sh again.
#
# What goes into the build is listed in Dockerfile.dockerignore next to this file.
FROM debian:trixie-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY agent agent
COPY tests/agent tests/agent
# The plugin's headers that need no HU Framework: test_plugin runs them against the agent.
COPY src src
# The image has no git, so the version comes from outside, as in build.sh.
ARG VERSION=unknown
RUN make -j"$(nproc)" agent-programs VERSION="$VERSION" \
    && mkdir /out \
    && cp build/agent-host/nbtagent build/agent-host/nbtagent-next build/agent-host/nbtctl build/agent-host/test_* /out/

# Python and the library nbtpkg signs with, for tests/make_packages.py, and nothing of the agent: built only as
# its own image (tools/bench.py testdata), so the agent's image below stays without Python, as a head unit is.
FROM debian:trixie-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258 AS vectors
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 python3-cryptography \
    && rm -rf /var/lib/apt/lists/*

# The agent's image: the last stage, so the one a plain `docker build` makes.
FROM debian:trixie-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258
COPY --from=build /out/ /opt/nbtagent/
# Fails the build here, and not a test later, if the image lacks a library the programs need.
RUN /opt/nbtagent/nbtagent --version && /opt/nbtagent/nbtagent-next --version && /opt/nbtagent/test_sha256

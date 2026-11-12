# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# The image tools/bench.py runs the agent in: nbtagent, nbtctl and the agent's test programs, built from this
# checkout for the image's own C library, on a Debian with a shell for the hooks and no compiler. Also
# nbtagent-next, the same agent built to say it is a later version, for the tests of the agent's own update.
#
#   tools/bench.py image        (docker build -f tools/bench/Dockerfile -t nbtapp-bench .)
#
# What goes into the build is listed in Dockerfile.dockerignore next to this file.
FROM debian:trixie-slim AS build
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

FROM debian:trixie-slim
COPY --from=build /out/ /opt/nbtagent/
# Fails the build here, and not a test later, if the image lacks a library the programs need.
RUN /opt/nbtagent/nbtagent --version && /opt/nbtagent/nbtagent-next --version && /opt/nbtagent/test_sha256

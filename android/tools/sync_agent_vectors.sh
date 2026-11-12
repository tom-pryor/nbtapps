#!/bin/sh
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Copies the files the head-unit agent and this app are both tested against out of the agent repo and into
# app/src/test/resources/agent. Run it after the agent repo's spec or test packages change; the unit test
# AgentVectorsInSyncTest fails while the two differ and the agent repo is there.
#
#   tools/sync_agent_vectors.sh [AGENT_REPO]      default: ../headunit
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
agent=${1:-"$here/../headunit"}
out="$here/app/src/test/resources/agent"

[ -f "$agent/tests/make_packages.py" ] || { echo "no agent repo at $agent" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/protocol" "$out/packages" "$out/keys"
cp "$agent/spec/json-profile.vectors" "$out/"
cp "$agent/spec/protocol/frames.vectors" "$agent/spec/protocol/session.vectors" "$agent/spec/protocol/constants.txt" \
    "$out/protocol/"
# Public halves only. The private halves stay in the agent repo, which signs the test packages.
cp "$agent/tests/keys/"*.pub "$out/keys/"
# The same input gives the same bytes, so this is the corpus the agent's own tests read.
python3 "$agent/tests/make_packages.py" "$out/packages" > /dev/null

cat > "$out/README" <<'TEXT'
Copied from the agent repo by tools/sync_agent_vectors.sh. Do not edit here: change the agent repo and copy again.

  json-profile.vectors      cases for the strict JSON profile (spec/json-profile.vectors)
  protocol/frames.vectors   good and bad frames (spec/protocol/frames.vectors)
  protocol/session.vectors  one conversation between a phone and the agent (spec/protocol/session.vectors)
  protocol/constants.txt    the names and numbers both sides must hold alike (spec/protocol/constants.txt)
  packages/                 good and deliberately broken .nbtpkg files, with vectors.json saying what each is
                            (tests/make_packages.py)
  keys/                     the public halves of the test keys that sign those packages (tests/keys)

Test keys only. Their private halves are public, so nothing signed with them is trustworthy: they are for
tests and for the debug build's sample catalogue, never for a build that leaves the test bench.
TEXT
echo "copied to $out"

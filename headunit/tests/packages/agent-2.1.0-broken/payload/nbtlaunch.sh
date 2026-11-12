#!/bin/sh
# Test package: stands in for the launcher of this version of the agent. An install has the shell read it and
# does not run it.
exec "${0%/*}/nbtagent" "$@"

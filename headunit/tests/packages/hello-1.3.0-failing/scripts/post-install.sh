#!/bin/sh
echo "post-install: failing on purpose"
echo "and on stderr" >&2
exit 3

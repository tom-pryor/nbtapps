#!/bin/sh
echo "post-install"
if [ -e "$NBT_DATA_DIR/fail" ]; then exit 3; fi

#!/bin/sh
# Logs where the manifest names them, as a program of the package would write them: 3,000 lines of text that
# deflate shrinks well in ${data}/log/logger.log, a short older one beside it, a file there that no entry names,
# and a crash report in ${tmp}, which the agent has made for the package.
mkdir -p "$NBT_DATA_DIR/log"
i=0
while [ $i -lt 3000 ]; do
    printf 'logger line %05d: the same few words, as a log has them, again and again\n' $i
    i=$((i + 1))
done > "$NBT_DATA_DIR/log/logger.log"
printf 'an older log\n' > "$NBT_DATA_DIR/log/older.log"
printf 'not a log: no entry names it\n' > "$NBT_DATA_DIR/log/notes.txt"
printf 'crash report\n' > "$NBT_TMP_DIR/crash.txt"

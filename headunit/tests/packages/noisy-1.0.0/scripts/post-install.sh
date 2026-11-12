#!/bin/sh
# 400 lines of 37 bytes with characters of two and three bytes, so that the pieces the agent reads from the pipe
# and the pieces it sends on end inside a character now and then; and one byte that is not UTF-8 at all.
i=0
while [ $i -lt 400 ]; do
    printf 'line %03d \303\244\303\266\303\274 \342\234\223 some more text\n' $i
    i=$((i + 1))
done
printf 'a byte that is not text: \377\n'

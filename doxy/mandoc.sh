#!/bin/sh
name="${1##*/}"
name="${name%.*}"
sane="$(echo "$name" | tr '+' 'x')"
sect="${1##*.}"
echo "\\page man_${sect}_$sane $name($sect) Manual

\htmlonly"
mandoc -Kutf-8 -Tutf8 -mdoc "$1" | "${0%/*}/mantohtml.awk"
echo '\endhtmlonly'

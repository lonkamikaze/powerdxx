#!/bin/sh
name="${1##*/}"
name="${name%.*}"
sane="$(echo "$name" | tr '+' 'x')"
echo -n "\\page man_${1##*.}_$sane "
mandoc -Kutf-8 -Tmarkdown -mdoc "$1" \
| sed -E '
# work around doxygen parsing issues with *
s/\*\*([^*]*)\*\*/<b>\1<\/b>/g
s/\*([^*]*)\*/<em>\1<\/em>/g
'

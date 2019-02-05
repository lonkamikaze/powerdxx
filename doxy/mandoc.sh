#!/bin/sh
set -fe

name="${1##*/}"
name="${name%.*}"
sane="$(echo "$name" | tr '+' 'x')"
sect="${1##*.}"
echo "\\page man_${sect}_$sane Manual $name($sect)
"
awk '{for (name in ENVIRON) gsub("%%" name "%%", ENVIRON[name])}1' "$1" | \
                                            mandoc -Kutf-8 -Tutf8 -mdoc | \
                        "${0%/*}/mantohtml.awk" -vESCAPE='\@&$#<>%".=|-'

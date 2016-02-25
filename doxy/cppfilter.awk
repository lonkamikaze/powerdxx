#!/usr/bin/awk -f
/\*\//{print;next}/\/\*\*/,/\*\//{sub(/^[ \t]*\* ?/,"")}1

#!/usr/bin/awk -f
#
# Adds github compatible labels to level 1 and 2 markdown headings
# in underline style.
#

#
# Substitute github references with doxygen references.
#
/\(#[_a-z0-9-]*\)/ {
	gsub(/\(#[_a-z0-9-]*\)/, "<__REF__&__REF__>")
	gsub(/<__REF__\(#/, "(@ref ")
	gsub(/\)__REF__>/, ")")
}

#
# If this line is underlining a heading, add a label to the previous
# line.
#
line && (/^===*$/ || /^---*$/) {
	id = tolower(line)
	gsub(/[^_a-z0-9]+/, "-", id)
	line = line " {#" id "}"
}

#
# Print the previous line, after a label might have been added.
#
{print line}

#
# Remember this line when scanning the next one for underlining.
#
{line = $0}

#
# Print the last line.
#
END { print }

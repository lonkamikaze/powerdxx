#!/usr/bin/awk -f
#
# Adds github compatible labels to level 1 and 2 markdown headings
# in underline style.
#

#
# Get file name prefix for references.
#
filename != FILENAME {
	filename = FILENAME
	# strip path
	"pwd" | getline path
	path = path "/"
	prefix = substr(filename, 1, length(path)) == path \
	         ? substr(filename, length(path) + 1) \
	         : filename
	sub(/\.[^.]*$/, "", prefix)
	gsub(/[^_a-z0-9]+/, "-", prefix)
	prefix = "md_" prefix "_"
}

#
# Substitute github references with doxygen references.
#
/\(#[_a-z0-9-]*\)/ {
	gsub(/\(#[_a-z0-9-]*\)/, "<__REF__&__REF__>")
	gsub(/<__REF__\(#/, "(@ref " prefix)
	gsub(/\)__REF__>/, ")")
}

#
# If this line is underlining a heading, add a label to the previous
# line.
#
line && (/^===*$/ || /^---*$/) {
	id = tolower(line)
	gsub(/[^- \t_a-z0-9]/, "", id) # remove undesired characters
	gsub(/[ \t]/, "-", id)         # replace white space with _
	line = line " {#" prefix id "}"
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

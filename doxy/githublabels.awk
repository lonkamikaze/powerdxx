#!/usr/bin/awk -f
#
# Adds github compatible labels to level 1 and 2 markdown headings
# in underline style.
#

#
# Get file name prefix for references.
#
# The prefix mimics the doxygen file naming scheme,
# e.g. `man/loadrec.1` becomse `man_1_loadrec`.
#
# Equivalently the prefix for `foo/bar.md` becomes the prefix `foo_md_bar_`.
# The trailing `_` exists as a separator to the internal reference label.
#
filename != FILENAME {
	filename = FILENAME
	# strip path
	"pwd -P" | getline path
	path = path "/"
	prefix = substr(filename, 1, length(path)) == path \
	         ? substr(filename, length(path) + 1) \
	         : filename
	# put the filename suffix behind the path but in front of
	# the file name
	sufx = prefix
	sub(/.*\./, "", sufx)               # get suffix
	sub(/\.[^.]*$/, "", prefix)         # strip suffix from filename
	sub(/[^\/]*$/, sufx "_&_", prefix)  # insert suffix
	# sanitise characters
	gsub(/[^_a-zA-Z0-9]+/, "_", prefix)
}

#
# Substitute github references with doxygen references.
#
/\(#[_a-z0-9-]*\)/ {
	gsub(/\[[^]]*\]\(#/, "&" prefix)
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

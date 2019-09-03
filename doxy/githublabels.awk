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
	gsub(/[^- \ta-z0-9]/, "", id)  # remove undesired characters
	gsub(/[ \t]/, "-", id)         # replace white space with -
	line = line " {#" prefix id "}"
}

#
# Doxygen up to at least 1.8.16 chokes on successive - in labels.
#
line ~ "#" prefix {
	cnt = split(line, a, "#" prefix)
	line = a[1]
	for (i = 2; i <= cnt; ++i) {
		while (a[i] ~ /^[-a-z0-9]*--[-a-z0-9]*/) {
			sub(/--+/, "-", a[i])
		}
		line = line "#" prefix a[i]
	}
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

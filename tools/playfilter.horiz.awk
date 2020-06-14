#!/usr/bin/awk -f
#
# Add a new column based on the values of the matched columns.
#
# The user is expected to set the following parameters via `-v var=value`
# command line arguments:
#
# | Variable  | Description                             |
# |-----------|-----------------------------------------|
# | `COLUMNS` | A `FS` separated list of column numbers |
# | `ALGO`    | An aggregating algorithm                |
#
# The `ALGO` variable should be set to one of:
#
# | Algorithm | Function                                  |
# |-----------|-------------------------------------------|
# | max       | The maximum value of the selected columns |
# | min       | The minimum value of the selected columns |
# | sum       | The sum of the selected columns           |
# | avg       | The mean over the selected columns        |
#

#
# Return the unit suffix of a column title.
#
# @param str
#	A column title
# @return
#	The unit suffix of the given title
#
function getunit(str, _i) {
	_i = match(str, RE_UNIT)
	return _i ? substr(str, _i) : ""
}

#
# Return the name in a column title without the unit suffix.
#
# @param str
#	A column title
# @return
#	The column name without the unit suffix
#
function getname(str, _i) {
	_i = match(str, RE_UNIT)
	return _i ? substr(str, 1, _i - 1) : str
}

#
# Read an indexed character from the front or back of a string.
#
# Characters are indexed starting from 1 for the first character
# and -1 for the last character.
#
# @param str
#	The string to get a character from
# @param i
#	The character index
# @return
#	The character at the given index
#
function getc(str, i) {
	if (i > 0) {
		return substr(str, i, 1)
	}
	return substr(str, length(str) + i + 1, 1)
}

#
# Extract a common prefix from the given strings.
#
# @param s0,s1
#	The strings to get a common prefix from
# @return
#	All front characters the strings have in common
#
function getprefix(s0, s1, _res, _len, _i, _c) {
	# prefix cannot be longer than the smaller string
	_len = length(s0) < length(s1) ? length(s0) : length(s1)
	_res = ""
	# accumulate prefix as long as characters match
	for (_i = 1; _i <= _len && (_c = getc(s0, _i)) == getc(s1, _i); ++_i) {
		# append newest character
		_res = _res _c
	}
	return _res
}

#
# Extract a common suffix from the given strings.
#
# @param s0,s1
#	The strings to get a common suffix from
# @return
#	All back characters the strings have in common
#
function getsuffix(s0, s1, _res, _len, _i, _c) {
	# suffix cannot be longer than the smaller string
	_len = length(s0) < length(s1) ? length(s0) : length(s1)
	_res = ""
	# accumulate suffix as long as characters match
	for (_i = 1; _i <= _len && (_c = getc(s0, -_i)) == getc(s1, -_i); ++_i) {
		# prepend newest character
		_res = _c _res
	}
	return _res
}

#
# Trim a prefix and suffix from a given string
#
# Does not verify that the prefix/suffix match the given string.
#
# @param str
#	The string to extract the middle slice from
# @param prefix,suffix
#	The head/tail to cut off the given string
# @return
#	The trimmed string
#
function getslice(str, prefix, suffix) {
	return substr(str, length(prefix) + 1,
	              length(str) - length(prefix) - length(suffix))
}

#
# Setup global constants.
#
BEGIN {
	# extract columns
	split(COLUMNS, columns)
	for (i in columns) { COLS[columns[i]] = 0 }
	delete columns

	# regex matching the unit at the end of a column title
	RE_UNIT = "\\[[^[]*\\]$"
}

#
# Create a new column.
#
{ $++NF }

#
# Add a column title for the new column.
#
# Setup the following variables and assemble them into a new column title:
#
# | Variable | Description                                    |
# |----------|------------------------------------------------|
# | `UNIT`   | The unit suffix at the end of the column title |
# | `PREFIX` | A common prefix of column titles               |
# | `SUFFIX` | A common suffix of column titles               |
# | `NAME`   | A comma separated list of title fragments      |
# | `TITLE`  | The title of the new column                    |
#
# E.g. given the columns `cpu.0.rec.load[MHz]` and `cpu.1.rec.load[MHz]`
# and the algorithm `ALGO=sum` would result in:
#
# | Variable | Value                          |
# |----------|--------------------------------|
# | `UNIT`   | `[MHz]`                        |
# | `PREFIX` | `cpu.`                         |
# | `SUFFIX` | `.rec.load`                    |
# | `NAME`   | `0,1`                          |
# | `TITLE`  | `sum(cpu.{0,1}.rec.load)[MHz]` |
#
NR == 1 {
	# initialise, UNIT, PREFIX and SUFFIX from the first column
	for (co in COLS) {
		UNIT = getunit($co)
		SUFFIX = PREFIX = getname($co)
		break
	}
	# aggregate common PREFIX and SUFFIX, check UNIT
	for (co in COLS) {
		unit = getunit($co)
		name = getname($co)
		PREFIX = getprefix(PREFIX, name)
		SUFFIX = getsuffix(SUFFIX, name)
		if (unit != UNIT) {
			print "error: h" ALGO " unit mismatch" > "/dev/stderr"
			print "hint:  " unit " != " UNIT       > "/dev/stderr"
			exit 1
		}
	}
	# assemble name slices in column order
	NAMES = ""
	for (co = 1; co < NF; ++co) {
		if (co in COLS) {
			NAMES = (NAMES ? NAMES "," : "") \
			        getslice($co, PREFIX, SUFFIX UNIT)
		}
	}
	# assign title
	$NF = TITLE = ALGO "(" PREFIX "{" NAMES "}" SUFFIX ")" UNIT
}

#
# Assign the maximum of the selected columns.
#
NR > 1 && ALGO == "max" {
	max = 0
	for (co in COLS) {
		max = (max >= $co ? max : $co)
	}
	$NF = max
}

#
# Assign the minimum of the selected columns.
#
NR > 1 && ALGO == "min" {
	for (co in COLS) {
		min = $co
		break
	}
	for (co in COLS) {
		min = (min <= $co ? min : $co)
	}
	$NF = min
}

#
# Assign the sum of the selected columns.
#
NR > 1 && ALGO == "sum" {
	sum = 0
	for (co in COLS) {
		sum += $co
	}
	$NF = sum
}

#
# Assign the mean of the selected columns.
#
NR > 1 && ALGO == "avg" {
	sum = 0
	cnt = 0
	for (co in COLS) {
		++cnt
		sum += $co
	}
	$NF = sum / cnt
}

# Print the current row.
1

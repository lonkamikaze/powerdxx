#!/usr/bin/awk -f
#
# Applies a moving average.
#
# The user is expected to set the following parameters via `-v var=value`
# command line arguments:
#
# | Variable  | Description                                              |
# |-----------|----------------------------------------------------------|
# | `COLUMNS` | A `FS` separated list of column numbers                  |
# | `PRE`     | The number of time slices to use before the sample point |
# | `POST`    | The number of time slices to use behind the sample point |
#
# Algorithm
# ---------
#
# The moving average algorithm takes the mean over a set of buffered
# values, the user provided PRE and POST values determine the amount
# of samples used.
#
# The following global variables control the algorithm and maintain
# its state:
#
# | Variable   | Type           | Description                          |
# |------------|----------------|--------------------------------------|
# | `PRE`      | int            | The target pre-sample buffer size    |
# | `POST`     | int            | The target post-sample buffer size   |
# | `SIZE`     | int            | The target buffer size (PRE + POST)  |
# | `SUM`      | int[co]        | The sums of buffered input columns   |
# | `SUM_TO`   | double         | Factor for conversion to fix-point   |
# | `SUM_FROM` | double         | Factor for conversion from fix-point |
# | `BUF_IN`   | double[li, co] | Buffered inputs                      |
# | `BUF_OUT`  | double[li, co] | Buffered outputs                     |
# | `BUF_SIZE` | int            | The actual buffer size               |
#
# ### Computation of the Moving Average
#
# The `SIZE = PRE + POST` value represents the desired buffer size.
# The actual buffer size is kept in `BUF_SIZE`. At the beginning of
# the file `BUF_SIZE` grows with every line read until it has reached
# `SIZE`.
#
# For each input column an output buffer is kept, untouched columns
# are put straight into the output buffer `BUF_OUT`. This buffer
# is kept around for the last `BUF_SIZE` lines of processed input.
# Filtered columns are first stored in the `BUF_IN` buffer. The per
# column `SUM` values represent the sum of all `BUF_IN` values in
# a fix-point representation.
#
# The mean is derived from the `SUM` for each line of output, once
# `BUF_SIZE` has reached `SIZE` the oldest value in `BUF_IN` is subtracted
# from the `SUM` and deleted with each additional line of input. The
# fix-point representation of `SUM` ensures that the sum does not
# accumulate drift through floating point rounding.
#
# ### Output
#
# How the mean is output depends on the values of `PRE` and `POST`.
# If only `PRE` is set every line of input causes an immediate output
# of the updated mean. If `POST` is set the output is delayed so
# *future values* become part of the mean.
#
# If `POST` is set the remaining lines of output have to be produced
# after reading the last input line. The `POST` buffer effect is
# reduced with every line of output until the last line, which only
# is made up of PRE buffer values.
#

#
# Setup global constants.
#
BEGIN {
	# extract columns
	split(COLUMNS, columns)
	for (i in columns) { SUM[columns[i]] = 0 }
	delete columns

	# sampling buffer properties
	PRE  = int(PRE)    # pre sample buffer size
	POST = int(POST)   # post sample buffer size
	SIZE = PRE + POST  # buffer size
	if (!SIZE) {
		print "error: movingavg: PRE + POST buffer must contain at least 1 value" > "/dev/stderr"
		exit 1
	}

	# use 20 fix-point fraction bits to calculate the buffer sum
	SUM_TO   = lshift(1, 20)
	SUM_FROM = 1. / SUM_TO
}

#
# Retrieve/Update the buffer sum for the given column.
#
# This function is the single point of access to the SUM buffer, it
# enforces integer arithmetic with a fixed number of fraction bits
# to avoid floating point drift.
#
# @param co
#	The column to retrieve/update
# @param offset
#	The change to the sum (optional)
# @param SUM
#	Global per column storage array
# @param SUM_TO,SUM_FROM
#	To and from fixed point conversion factors
# @return
#	The current buffer sum for the given column
#
function sum(co, offset) {
	return (SUM[co] += int(offset * SUM_TO + .5)) * SUM_FROM
}

#
# Print a buffered line, and trim the output buffer.
#
# Overwrites the input fields.
#
# @param NR,POST
#	The current line and the output line offset
# @param BUF_OUT
#	The output buffer to output a line from and trim
#
function printbuf(_li, _co) {
	# Fetch columns from output buffer
	_li = NR - POST
	for (_co = 1; _co <= NF; ++_co) {
		$_co = BUF_OUT[_li, _co]
		delete BUF_OUT[_li, _co]
	}
	print
}

#
# Update the buffer size until the target is reached.
#
NR <= SIZE { BUF_SIZE = NR }

#
# Update buffers.
#
# - Forward unaffected columns to the output buffer
# - Update BUF_IN, SUM and BUF_OUT for managed columns
# - Trim obsolete input buffer
#
{
	for (co = 1; co <= NF; ++co) {
		if (co in SUM) {
			# remove buffered input moving out of the
			# buffering window
			sum(co, -BUF_IN[NR - SIZE, co])
			delete  BUF_IN[NR - SIZE, co]

			# add latest value to the input buffer
			BUF_IN[NR, co] = $co
			# add latest value to the buffer sum and
			# update the output buffer
			BUF_OUT[NR - POST, co] = sum(co, +$co) / BUF_SIZE
		} else {
			# forward column to the output buffer
			BUF_OUT[NR, co] = $co
		}
	}
}

#
# For every line of input, produce a line of output, starting when
# the POST buffer is full.
#
NR > POST { printbuf() }

#
# Flush the remaining post buffer.
#
END {
	for (o = 1; o <= POST; ++o) {
		++NR
		--BUF_SIZE
		for (co in SUM) {
			# remove buffered input moving out of the
			# buffering window and update the output buffer
			BUF_OUT[NR - POST, co] = sum(co, -BUF_IN[NR - SIZE, co]) / BUF_SIZE
			delete  BUF_IN[NR - SIZE, co]
		}
		printbuf()
	}
}

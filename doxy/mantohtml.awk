#!/usr/bin/awk -f
#
# Takes a terminal formatted manual page and generates an identical
# looking HTML <pre> block.
#
# This script only supports UTF-8 encoding.
#

BEGIN {
	# The backspace character is used to combine characters
	BS = "\x08"

	# prepare requested escapes
	n = u8chars(escape, ESCAPE)
	for (i = 1; i <= n; ++i) {
		SUB[escape[i]] = "\\" escape[i]
	}

	# special HTML character substitutions
	SUB["&"] = "&amp;"
	SUB["<"] = "&lt;"
	SUB[">"] = "&gt;"

	# formatting flags
	FMT_REGULAR    = 0
	FMT_BOLD       = 1
	FMT_UNDERLINE  = 2

	# formatting selectors
	SELECT["_"] = FMT_UNDERLINE

	# opening tags
	OPEN[FMT_BOLD]                 = "<b>"
	OPEN[FMT_UNDERLINE]            = "<u>"
	OPEN[FMT_BOLD + FMT_UNDERLINE] = "<b><u>"

	# closing tags
	CLOSE[FMT_BOLD]                 = "</b>"
	CLOSE[FMT_UNDERLINE]            = "</u>"
	CLOSE[FMT_BOLD + FMT_UNDERLINE] = "</u></b>"

	print "<pre>"
}

END {
	print "</pre>"
}

function u8error(msg)
{
	printf("utf-8 error:%d:%d:%d: %s\n\n%s\n% " p "s\n", NR, p, i, msg, str, "^") > "/dev/stderr"
}

function u8chars(uchars, str,
                 n, chars, p, mbyte, i)
{
	# split characters
	delete chars
	n = split(str, chars, "")

	delete uchars
	p = 1      # index of the next unicode character
	mbyte = 0  # expected multi-byte character bytes
	for (i = 1; i <= n; ++i) {
		# no leading bits, 1 byte character
		if (mbyte)
		{
			if (chars[i] < "\x80" || chars[i] >= "\xc0") {
				u8error("unexected byte in multibyte character")
				mbyte = 0
				continue
			}
			uchars[p] = uchars[p] chars[i]
			p += !--mbyte
		}
		# regular 1 byte characters
		else if (chars[i] < "\x80") {
			uchars[p++] = chars[i]
		}
		# 2 leading bits, merge 2 bytes
		else if (chars[i] >= "\xc0" && chars[i] < "\xe0") {
			uchars[p] = chars[i]
			mbyte = 1
		}
		# 3 leading bits, merge 3 bytes
		else if (chars[i] >= "\xe0" && chars[i] < "\xf0") {
			uchars[p] = chars[i]
			mbyte = 2
		}
		# 4 byte characters
		else if (chars[i] >= "\xf0" && chars[i] < "\xf8") {
			uchars[p] = chars[i]
			mbyte = 3
		}
		# error
		else
		{
			u8error("byte is not a character")
		}
	}
	return p - 1
}

{
	#
	# merge utf-8 multibyte chars
	#
	delete uchars
	n = u8chars(uchars, $0)

	#
	# detect per character formatting
	#
	delete format
	delete ochars

	olen = 0
	for (i = 1; i <= n; ++i) {
		++olen
		# check all formatting characters
		for (; uchars[i + 1] == BS; i += 2) {
			format[olen] = or(format[olen], SELECT[uchars[i]])
			# bold is any match in the chain of characters,
			# so check the current one against all the
			# following ones
			for (p = i + 2; uchars[p - 1] == BS; p += 2) {
				format[olen] = or(format[olen], uchars[i] == uchars[p] ? FMT_BOLD : FMT_REGULAR)
			}
		}
		# add character
		ochars[olen] = uchars[i]
	}

	#
	# replace special characters
	#
	for (i = 1; i <= olen; ++i) {
		ochars[i] in SUB && ochars[i] = SUB[ochars[i]]
	}

	#
	# output with HTML formatting
	#
	$0 = ""

	last = FMT_REGULAR
	for (i = 1; i <= olen; ++i) {
		if (format[i] != last) {
			$0 = $0 CLOSE[last] OPEN[format[i]]
		}
		$0 = $0 ochars[i]
		last = format[i]
	}
	$0 = $0 CLOSE[last]

	print
}


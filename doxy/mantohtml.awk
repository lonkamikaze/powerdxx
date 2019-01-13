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

	# Special character substitutions
	SUB["&"] = "&amp;"
	SUB["<"] = "&lt;"
	SUB[">"] = "&gt;"

	print "<pre>"
}

END {
	print "</pre>"
}

function u8error(msg)
{
	printf("utf-8 error:%d:%d:%d: %s\n\n%s\n% " p "s\n", NR, p, i, msg, $0, "^")
	exit 1
}

{
	# split characters
	delete chars

	n = split($0, chars, "")

	#
	# merge utf-8 multibyte chars
	#
	delete uchars

	p = 1
	mbyte = 0
	for (i = 1; i <= n; ++i) {
		# no leading bits, 1 byte characte
		if (mbyte)
		{
			if (chars[i] < "\x80" || chars[i] >= "\xc0") {
				u8error("unexected byte in multibyte character")
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
	n = p

	#
	# replace special characters
	#
	for (i = 1; i <= n; ++i) {
		uchars[i] in SUB && uchars[i] = SUB[uchars[i]]
	}

	#
	# detect per character formatting
	#
	delete hide
	delete formatted
	delete bold
	delete underline
	delete strike

	for (i = 1; i <= n; ++i) {
		if (uchars[i] != BS) {
			continue
		}
		# hide composed characters
		hide[i - 1] = hide[i] = 1
		# detect formatting
		bold[i + 1] = (uchars[i - 1] == uchars[i + 1]) || bold[i - 1];
		underline[i + 1] = !bold[i + 1] && (uchars[i - 1] == "_") || underline[i - 1];
		strike[i + 1] = !bold[i + 1] && (uchars[i - 1] == "-") || strike[i - 1];
	}

	#
	# output with HTML formatting
	#
	$0 = ""

	bmode = 0
	umode = 0
	smode = 0
	for (i = 1; i <= n; ++i) {
		if (hide[i]) {
			continue
		}
		if (!bmode && bold[i]) {
			$0 = $0 "<b>"
			bmode = 1
		}
		if (!umode && underline[i]) {
			$0 = $0 "<u>"
			umode = 1
		}
		if (!smode && strike[i]) {
			$0 = $0 "<strike>"
			smode = 1
		}
		if (smode && !strike[i]) {
			$0 = $0 "</strike>"
			smode = 0
		}
		if (umode && !underline[i]) {
			$0 = $0 "</u>"
			umode = 0
		}
		if (bmode && !bold[i]) {
			$0 = $0 "</b>"
			bmode = 0
		}
		$0 = $0 uchars[i]
	}
	if (smode && !strike[i]) {
		$0 = $0 "</strike>"
	}
	if (umode && !underline[i]) {
		$0 = $0 "</u>"
	}
	if (bmode && !bold[i]) {
		$0 = $0 "</b>"
	}

	print
}


#!/bin/sh
name="${1##*/}"
name="${name%.*}"
sane="$(echo "$name" | tr '+' 'x')"
echo "\\page man_${1##*.}_$sane $name(${1##*.})

\\brief Manual page for \`$name(${1##*.})\`.

---

<pre>"
groff -Tutf8 -mdoc "$1" | awk '
{
	gsub(/—/, "\\&mdash;")
	gsub(/‐/, "-")

	cnt = split($0, a, /\[/)
	for (i = 1; i <= cnt; ++i) {
		if (a[i] ~ /^1m/) {
			a[i - 1] = a[i - 1] "<b>"
			for (q = i + 1; q <= cnt; ++q) {
				if (a[q] ~ /^(0|4|22)m/) {
					a[q - 1] = a[q - 1] "</b>"
					break
				}
			}
		}
		if (a[i] ~ /^4m/) {
			a[i - 1] = a[i - 1] "<em>"
			for (q = i + 1; q <= cnt; ++q) {
				if (a[q] ~ /^(0|24)m/) {
					a[q - 1] = a[q - 1] "</em>"
					break
				}
			}
		}
	}
	$0 = a[1]
	for (i = 2; i <= cnt; ++i) {
		$0 = $0 "[" a[i]
	}

	gsub(/\[[0-9]+m/, "")
}
1'
echo "</pre>"

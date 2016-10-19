#!/bin/sh
. "${0%/*}/common.sh" "$@"
sed "${sedprog}" | while read type src tgt; do
	echo rm "${DESTDIR:+${DESTDIR%/}/}${tgt}"
	rm "${DESTDIR:+${DESTDIR%/}/}${tgt}"
	while rmdir "${DESTDIR:+${DESTDIR%/}/}${tgt%/*}" 2> /dev/null; do
		tgt="${tgt%/*}"
	done
done

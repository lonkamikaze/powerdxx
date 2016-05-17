#!/bin/sh
set -f
export "$@"
sedprog=
for envvar in CURDIR OBJDIR PREFIX DOCSDIR; do
	eval "sedprog=\"${sedprog}${sedprog:+;}s!%%${envvar}%%!\${${envvar}%/}!g\""
done
IFS=':'
: ${BSD_INSTALL_PROGRAM=install  -s -m 555}
: ${BSD_INSTALL_MAN=install  -m 444}
: ${BSD_INSTALL_SCRIPT=install  -m 555}
: ${GZIP_CMD=gzip -nf9}
sed "${sedprog}" | while read type src tgt; do
	eval echo "\${BSD_INSTALL_${type}} ${src} ${DESTDIR:+${DESTDIR%/}/}${tgt}"
	mkdir -p ${DESTDIR:+${DESTDIR%/}/}${tgt%/*}
	eval eval "\${BSD_INSTALL_${type}} ${src} ${DESTDIR:+${DESTDIR%/}/}${tgt%.gz}"
	case ${type} in
	MAN | SCRIPT)
		sed -i '' "${sedprog}" "${DESTDIR:+${DESTDIR%/}/}${tgt%.gz}"
	;;
	esac
	if [ -z "${tgt##*.gz}" ]; then
		eval ${GZIP_CMD} "${DESTDIR:+${DESTDIR%/}/}${tgt%.gz}"
	fi
done

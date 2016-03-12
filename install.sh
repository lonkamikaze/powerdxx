#!/bin/sh
set -f
export "$@"
sedprog=
for envvar in CURDIR OBJDIR PREFIX DOCSDIR; do
	eval "sedprog=\"${sedprog}${sedprog:+;}s!%%${envvar}%%!\${${envvar}%/}!g\""
done
IFS=':'
sed "${sedprog}" | while read type src tgt; do
	eval echo "\${BSD_INSTALL_${type}} ${src} ${DESTDIR:+${DESTDIR%/}/}${tgt}"
	mkdir -p ${DESTDIR:+${DESTDIR%/}/}${tgt%/*}
	eval $(eval echo "\${BSD_INSTALL_${type}} ${src} ${DESTDIR:+${DESTDIR%/}/}${tgt}")
	case ${type} in
	MAN | SCRIPT)
		sed -i '' "${sedprog}" "${DESTDIR:+${DESTDIR%/}/}${tgt}"
	;;
	esac
done

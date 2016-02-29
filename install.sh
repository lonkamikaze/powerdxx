#!/bin/sh
set -f
export "$@"
IFS=':'
while read type src tgt; do
	test -z "${DOCS}" -a "${type}" == "MAN" && continue
	eval echo "\${BSD_INSTALL_${type}} ${src} ${DESTDIR:+${DESTDIR%/}/}${tgt}"
	mkdir -p ${DESTDIR:+${DESTDIR%/}/}${tgt%/*}
	eval $(eval echo "\${BSD_INSTALL_${type}} ${src} ${DESTDIR:+${DESTDIR%/}/}${tgt%.gz}")
	case ${type} in
	MAN | SCRIPT)
		eval "sed -i '' ${SUB} ${DESTDIR:+${DESTDIR%/}/}${tgt%.gz}"
	;;
	esac
done

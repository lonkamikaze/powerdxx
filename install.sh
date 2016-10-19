#!/bin/sh
. "${0%/*}/common.sh" "$@"
sed "${sedprog}" | while read type src tgt; do
	eval echo "\${BSD_INSTALL_${type}} \"\${src}\" \"\${DESTDIR:+\${DESTDIR%/}/}\${tgt}\""
	mkdir -p "${DESTDIR:+${DESTDIR%/}/}${tgt%/*}"
	eval eval "\${BSD_INSTALL_${type}} \"\${src}\" \"\${DESTDIR:+\${DESTDIR%/}/}\${tgt%.gz}\""
	case ${type} in
	MAN | SCRIPT)
		sed -i '' "/#RM\$/d${sedprog}" "${DESTDIR:+${DESTDIR%/}/}${tgt%.gz}"
	;;
	esac
	if [ -z "${tgt##*.gz}" ]; then
		eval ${GZIP_CMD} "${DESTDIR:+${DESTDIR%/}/}${tgt%.gz}"
	fi
done

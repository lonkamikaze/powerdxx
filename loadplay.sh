#!/bin/sh
set -f
preload="%%PREFIX%%/lib/libloadplay.so"
while [ "${1#-}" != "$1" ]; do
	case "$1" in
	-h | --help)
		/bin/cat 1>&2 << USAGE
usage: loadplay [-h] [-i file] [-o file] command ...

        -h, --help          Show usage and exit
        -i, --input   file  Input from file
        -o, --output  file  Output to file
USAGE
		return 0
	;;
	-i | --input)
		exec <"$2"
		shift 2
	;;
	-i*)
		exec <"${1#-i}"
		shift
	;;
	-o | --output)
		exec >"$2"
		shift 2
	;;
	-o*)
		exec <"${1#-o}"
		shift
	;;
	- | -? | --*)
		echo "loadplay: unknown option: $1" >&2
		"$0" --help
		return 1
	;;
	-?*)
		arg="$1"
		shift
		set -- "${arg%%${arg#-?}}" "-${arg#-?}" "$@"
	;;
	esac
done
if [ -z "$1" ]; then
	echo "loadplay: command missing" >&2
	"$0" --help
	return 1
fi
exec /usr/bin/env LD_PRELOAD="$preload" "$@"

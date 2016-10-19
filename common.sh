set -f
export "$@"
sedprog=
for envvar in CURDIR OBJDIR PREFIX DOCSDIR; do
	eval "sedprog=\"${sedprog};s!%%${envvar}%%!\${${envvar}%/}!g\""
done
IFS=':'
: ${BSD_INSTALL_PROGRAM=install  -s -m 555}
: ${BSD_INSTALL_MAN=install  -m 444}
: ${BSD_INSTALL_SCRIPT=install  -m 555}
: ${BSD_INSTALL_LIB=install  -s -m 444}
: ${BSD_INSTALL_SYMLINK=install  -l s}
: ${GZIP_CMD=gzip -nf9}

FLAGS=     -std=c++14 -Wall -Werror -pedantic
DBGFLAGS=  -O0 -g -DEBUG
PFLAGS=    -fstack-protector -fsanitize=undefined -fsanitize-undefined-trap-on-error
GXX5=      g++5
GXX5FLAGS= -Wl,-rpath=/usr/local/lib/gcc5

PREFIX?=   /usr/local
DOCSDIR?=  ${PREFIX}/share/doc/powerdxx

BINS=      src/powerd++.cpp src/loadrec.cpp
SOS=       src/loadplay.cpp
SRCS!=     cd ${.CURDIR} && find src/ -name \*.cpp
TARGETS=   ${BINS:C/.*\///:C/\.cpp$//} ${SOS:C/.*\///:C/\.cpp$/.so/:C/^/lib/}
TMP!=      cd ${.CURDIR} && \
           env MKDEP_CPP_OPTS="-MM -std=c++14" mkdep ${SRCS}
RELEASE!=  git tag 2>&- | tail -n1 || :

# Build
all: ${TARGETS}

.sinclude ".depend"

# Building
loadplay.o:
	${CXX} ${CXXFLAGS} -fPIC -c ${.IMPSRC} -o ${.TARGET}

# Linking
#
# | Flag      | Targets           | Why                                    |
# |-----------|-------------------|----------------------------------------|
# | -lutil    | powerd++          | Required for pidfile_open() etc.       |
# | -lpthread | libloadplay.so    | Uses std::thread                       |

powerd++: ${.TARGET}.o clas.o
	${CXX} ${CXXFLAGS} ${.ALLSRC} -lutil -o ${.TARGET}

loadrec: ${.TARGET}.o clas.o
	${CXX} ${CXXFLAGS} ${.ALLSRC} -o ${.TARGET}

libloadplay.so: ${.TARGET:C/^lib//:C/\.so$//}.o
	${CXX} ${CXXFLAGS} ${.ALLSRC} -lpthread -shared -o ${.TARGET}

# Combinable build targets
.ifmake(debug)
CXXFLAGS=  ${DBGFLAGS}
.endif
CXXFLAGS+= ${FLAGS}
.ifmake(g++5)
CXX=       ${GXX5}
CXXFLAGS+= ${GXX5FLAGS}
.endif
.ifmake(paranoid)
CXXFLAGS+= ${PFLAGS}
.endif

debug g++5 paranoid: all

# Install
install: ${TARGETS}

install deinstall: ${.TARGET}.sh pkg.tbl
	@${.CURDIR}/${.TARGET}.sh < ${.CURDIR}/pkg.tbl \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"

# Clean
clean:
	rm -f *.o ${TARGETS}

# Documentation
doc::
	rm -rf ${.TARGET}/*
	cd "${.CURDIR}" && (cat doxy/doxygen.conf; \
		echo PROJECT_NUMBER='"${RELEASE}"') | \
		env PREFIX="${PREFIX}" doxygen -

doc/latex/refman.pdf: doc
	cd "${.CURDIR}" && cd "${.TARGET:H}" && ${MAKE}

gh-pages: doc doc/latex/refman.pdf
	rm -rf ${.TARGET}/*
	cp -R ${.CURDIR}/doc/html/* ${.CURDIR}/doc/latex/refman.pdf ${.TARGET}/

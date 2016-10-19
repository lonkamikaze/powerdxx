CXXFLAGS+= -std=c++11 -Wall -Werror -pedantic
CFLAGS+=   -std=c11 -Wall -Werror -pedantic
PREFIX?=   /usr/local
DOCSDIR?=  ${PREFIX}/share/doc/powerdxx

SRCS=      src/powerd++.cpp src/loadrec.cpp
SOS=       src/loadplay.cpp
TARGETS=   ${SRCS:C/.*\///:C/\.cpp$//} ${SOS:C/.*\///:C/\.cpp$/.so/:C/^/lib/}
TMP!=      cd ${.CURDIR} && mkdep ${SRCS} ${SOS}

all: ${TARGETS}

.sinclude ".depend"

powerd++: ${.TARGET}.o
	${CXX} -lutil ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

loadrec: ${.TARGET}.o
	${CXX} ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

libloadplay.so: ${.TARGET:C/^lib//:C/\.so$//}.o
	${CXX} -lpthread -shared ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

loadplay.o:
	${CXX} -c ${CXXFLAGS} -fPIC -o ${.TARGET} ${.IMPSRC}

install: ${TARGETS}

install deinstall: ${.TARGET}.sh pkg.tbl
	@${.CURDIR}/${.TARGET}.sh < ${.CURDIR}/pkg.tbl \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"

clean:
	rm -f *.o ${TARGETS}

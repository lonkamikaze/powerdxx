CXXFLAGS+= -std=c++11 -Wall -Werror -pedantic
PREFIX?=   /usr/local
DOCSDIR?=  ${PREFIX}/share/doc/powerdxx

SRCS=      src/powerd++.cpp src/loadrec.cpp
TARGETS=   ${SRCS:C/.*\///:C/\.cpp$//}
TMP!=      cd ${.CURDIR} && mkdep ${SRCS}

all: ${TARGETS}

.for file in ${SRCS}
${file:C/.*\///:C/\.cpp$//}.o: ${file}
.endfor

.sinclude ".depend"

powerd++: ${.TARGET}.o
	${CXX} -lutil ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

loadrec: ${.TARGET}.o
	${CXX} ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

install: install.sh pkg.tbl powerd++
	@${.CURDIR}/install.sh < ${.CURDIR}/pkg.tbl \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"

clean:
	rm *.o ${TARGETS}

CXXFLAGS+= -std=c++11 -Wall -Werror
PREFIX?=   /usr/local
DOCSDIR?=  ${PREFIX}/share/doc/powerdxx

all: powerd++

powerd++: powerd++.o
	${CXX} -lutil ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

powerd++.o: src/powerd++.cpp src/Options.hpp

install: install.sh pkg.tbl powerd++
	@${.CURDIR}/install.sh < ${.CURDIR}/pkg.tbl \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"

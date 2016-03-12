CXXFLAGS+= -std=c++11 -Wall -Werror

all: powerd++

powerd++: powerd++.o
	${CXX} -lutil ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

powerd++.o: src/powerd++.cpp src/Options.hpp

.if defined(PREFIX) && defined(DOCSDIR)
install: install.sh pkg.tbl powerd++
	@${.CURDIR}/install.sh < ${.CURDIR}/pkg.tbl \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"
.endif

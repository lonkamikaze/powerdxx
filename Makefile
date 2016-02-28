CXXFLAGS+= -std=c++11 -Wall -Werror

all: powerd++

powerd++: powerd++.o
	${CXX} -lutil ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

powerd++.o: src/powerd++.cpp src/Options.hpp

.if defined(SUB)
install: install.sh pkg.tbl powerd++
	@sed ${SUB} pkg.tbl | ./install.sh DESTDIR="${DESTDIR}"
.endif

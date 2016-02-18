CXXFLAGS+= -std=c++11 -Wall -Werror

powerd++.o: src/powerd++.cpp src/Options.hpp

powerd++: powerd++.o
	${CXX} -lutil ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}


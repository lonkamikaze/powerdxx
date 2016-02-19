CXXFLAGS+= -std=c++11 -Wall -Werror

powerd++: powerd++.o
	${CXX} -lutil ${CXXFLAGS} -o ${.TARGET} ${.ALLSRC}

powerd++.o: src/powerd++.cpp src/Options.hpp


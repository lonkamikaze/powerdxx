FLAGS=     -std=${STD} -Wall -Werror -pedantic
STD=       c++14
DBGFLAGS=  -O0 -g -DEBUG
PFLAGS=    -fstack-protector -fsanitize=undefined -fsanitize-undefined-trap-on-error

PREFIX?=   /usr/local
DOCSDIR?=  ${PREFIX}/share/doc/powerdxx

BINS=      src/powerd++.cpp src/loadrec.cpp
SOS=       src/libloadplay.cpp
SRCS!=     cd ${.CURDIR} && find src/ -name \*.cpp
TARGETS=   ${BINS:C/.*\///:C/\.cpp$//} ${SOS:C/.*\///:C/\.cpp$/.so/}
TMP!=      cd ${.CURDIR} && \
           env MKDEP_CPP_OPTS="-MM -std=${STD}" mkdep ${SRCS}
RELEASE!=  git tag -l --sort=-taggerdate 2>&- | head -n1 || :

# Build
all: ${TARGETS}

.sinclude ".depend"

# Building
libloadplay.o:
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

libloadplay.so: ${.TARGET:C/\.so$//}.o
	${CXX} ${CXXFLAGS} ${.ALLSRC} -lpthread -shared -o ${.TARGET}

# Combinable build targets
.ifmake(debug)
CXXFLAGS=  ${DBGFLAGS}
.endif
CXXFLAGS+= ${FLAGS}
.ifmake(paranoid)
CXXFLAGS+= ${PFLAGS}
.endif

debug paranoid: all

# Install
install: ${TARGETS}

install deinstall: pkg/${.TARGET:C,.*/,,}.sh pkg/files
	@${.CURDIR}/pkg/${.TARGET:C,.*/,,}.sh < ${.CURDIR}/pkg/files \
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

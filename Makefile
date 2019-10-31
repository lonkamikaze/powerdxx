FLAGS=     -std=${STD} -Wall -Werror -pedantic
STD=       c++17
DBGFLAGS=  -O0 -g -DEBUG
PFLAGS=    -fstack-protector -fsanitize=undefined -fsanitize-undefined-trap-on-error
TESTBUILDS=clang++90 clang++80 clang++70 g++9

PREFIX?=   /usr/local
DOCSDIR?=  ${PREFIX}/share/doc/powerdxx

BINCPPS=   src/powerd++.cpp src/loadrec.cpp src/loadplay.cpp
SOCPPS=    src/libloadplay.cpp
SRCFILES!= cd ${.CURDIR} && find src/ -type f
CPPS=      ${SRCFILES:M*.cpp}
TARGETS=   ${BINCPPS:T:.cpp=} ${SOCPPS:T:.cpp=.so}
RELEASE!=  git tag -l --sort=-taggerdate 2>&- | head -n1 || date -uI
COMMITS!=  git rev-list --count HEAD "^${RELEASE}" 2>&- || echo 0
VERSION=   ${RELEASE}${COMMITS:C/^/+c/:N+c0}

# Build
all: ${TARGETS}

# Create .depend
.depend: ${SRCFILES}
	cd ${.CURDIR} && env MKDEP_CPP_OPTS="-MM -std=${STD}" mkdep ${CPPS}

.if ${.MAKE.LEVEL} == 0
TMP!=      cd ${.CURDIR} && ${MAKE} .depend
.endif
# Usually .depend is read implicitly after parsing the Makefile,
# but it's needed before that to generate the testbuild/* targets.
.include ".depend"

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

loadrec loadplay: ${.TARGET}.o clas.o
	${CXX} ${CXXFLAGS} ${.ALLSRC} -o ${.TARGET}

libloadplay.so: ${.TARGET:.so=.o}
	${CXX} ${CXXFLAGS} ${.ALLSRC} -lpthread -shared -o ${.TARGET}

# Combinable build targets
.ifmake(debug)
CXXFLAGS=  ${DBGFLAGS}
.endif
CXXFLAGS+= ${FLAGS}
.ifmake(paranoid)
CXXFLAGS+= ${PFLAGS}
.endif

debug paranoid: ${.TARGETS:Ndebug:Nparanoid:S/^$/all/W}

# Install
install: ${TARGETS}

install deinstall: pkg/${.TARGET:C,.*/,,}.sh pkg/files
	@${.CURDIR}/pkg/${.TARGET:C,.*/,,}.sh < ${.CURDIR}/pkg/files \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"

# Clean
clean:
	rm -f *.o ${TARGETS}

# Test-build with supported compilers
#
# Provides the following targets:
#
# | Target                 | Description                              |
# |------------------------|------------------------------------------|
# | testbuild              | Builds the default target for all builds |
# | testbuild/TARGET       | Builds TARGET for all builds             |
# | testbuild/BUILD        | Builds the default target for BUILD      |
# | testbuild/BUILD/TARGET | Build TARGET for BUILD                   |
#
# Valid builds are listed in ${TESTBUILDS}.
# Valid targets are all targets occurring before this block.
TBTARGETS:=    ${.ALLTARGETS:N*/*:N$$*}

# Build default target
${TESTBUILDS:S,^,testbuild/,}::
	@mkdir -p "${.TARGET}"
	@echo [${.TARGET}]: ${MAKE} ${MAKEFLAGS:N.*}
	@(cd "${.CURDIR}" && \
	  ${MAKE} MAKEOBJDIR="${.CURDIR}/obj/${.TARGET}" CXX="${.TARGET:T}")
	@rmdir -p "${.TARGET}" 2> /dev/null ||:

# Build default target for all
testbuild: ${TESTBUILDS:S,^,testbuild/,}

# Build specific target
.for target in ${TBTARGETS}
${TESTBUILDS:S,^,testbuild/,:S,$,/${target},}::
	@mkdir -p "${.TARGET:H}"
	@echo [${.TARGET:H}]: ${MAKE} ${.TARGET:T} ${MAKEFLAGS:N.*}
	@(cd "${.CURDIR}" && \
	  ${MAKE} MAKEOBJDIR="${.CURDIR}/obj/${.TARGET:H}" CXX="${.TARGET:H:T}" \
	          ${.TARGET:T})
	@rmdir -p "${.TARGET:H}" 2> /dev/null ||:
.endfor

# Build specific target for all
${TBTARGETS:S,^,testbuild/,}: ${TESTBUILDS:S,^,testbuild/,:S,$,/${.TARGET:T},}

# Alias all testbuild/* targets to tb/
${.ALLTARGETS:Mtestbuild/*:S,testbuild/,tb/,}: ${.TARGET:S,tb/,testbuild/,}
tb: testbuild

# Documentation
doc::
	rm -rf ${.TARGET}/*
	cd "${.CURDIR}" && (cat doxy/doxygen.conf; \
		echo PROJECT_NUMBER='"${VERSION}"') | \
		env PREFIX="${PREFIX}" doxygen -

doc/latex/refman.pdf: doc
	cd "${.CURDIR}" && cd "${.TARGET:H}" && ${MAKE}

gh-pages: doc doc/latex/refman.pdf
	rm -rf ${.TARGET}/*
	cp -R ${.CURDIR}/doc/html/* ${.CURDIR}/doc/latex/refman.pdf ${.TARGET}/

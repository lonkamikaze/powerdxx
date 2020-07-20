FLAGS=         -std=${STD} -Wall -Werror -pedantic
STD=           c++17
DBGFLAGS=      -O0 -g -DEBUG
PFLAGS=        -fstack-protector -fsanitize=undefined -fsanitize-undefined-trap-on-error
TESTBUILDS=    clang++10 clang++90 clang++80 g++9

PREFIX?=       /usr/local
DOCSDIR?=      ${PREFIX}/share/doc/powerdxx

BINCPPS=       src/powerd++.cpp src/loadrec.cpp src/loadplay.cpp
SOCPPS=        src/libloadplay.cpp
SRCFILES!=     cd ${.CURDIR} && find src/ -type f
HPPS=          ${SRCFILES:M*.hpp}
CPPS=          ${SRCFILES:M*.cpp}
TARGETS=       ${BINCPPS:T:.cpp=} ${SOCPPS:T:.cpp=.so}
CLEAN=         *.o *.pch ${TARGETS}

PKGVERSION=    ${.CURDIR:T:C/[^-]*-//:M[0-9]*.[0-9]*.[0-9]*}
GITVERSION.sh= git describe 2>&- || :
GITRELEASE=    ${GITRELEASE.sh:sh}
GITCOMMITS=    ${GITCOMMITS.sh:sh}
GITVERSION=    ${GITVERSION.sh:sh}
VERSIONLIST=   ${GITVERSION} ${PKGVERSION} unknown
VERSION=       ${VERSIONLIST:[1]}

INFO=          VERSION GITVERSION GITHASH PKGVERSION TARGETS \
               CXX CXXFLAGS CXXVERSION UNAME_A
GITHASH.sh=    git log -1 --pretty=%H 2>&- || :
GITHASH=       ${GITHASH.sh:sh}
CXXVERSION.sh= ${CXX} --version
CXXVERSION=    ${CXXVERSION.sh:sh}
UNAME_A.sh=    uname -a
UNAME_A=       ${UNAME_A.sh:sh}

# Build
all: ${TARGETS}

# Create .depend
.depend: ${SRCFILES}
	cd ${.CURDIR} && env MKDEP_CPP_OPTS="-MM -std=${STD}" mkdep ${CPPS}

.if !make(.depend)
TMP!=      cd ${.CURDIR} && ${MAKE} .depend
# Usually .depend is read implicitly after parsing the Makefile,
# but it's needed before that to generate the testbuild/* targets.
.include "${.CURDIR}/.depend"
.endif

# Building/Linking
#
# | Flag      | Targets           | Why                                    |
# |-----------|-------------------|----------------------------------------|
# | -lutil    | powerd++          | Required for pidfile_open() etc.       |
# | -lpthread | libloadplay.so    | Uses std::thread                       |

CXXFLAGS.libloadplay.o=  -fPIC
CXXFLAGS.libloadplay.so= -lpthread -shared
CXXFLAGS.powerd++ =      -lutil

${TARGETS:M*.so}: mk-binary ${.TARGET:.so=.o}
${TARGETS:N*.so}: mk-binary ${.TARGET}.o clas.o utility.o

mk-binary: .USE
	${CXX} ${CXXFLAGS} ${.ALLSRC} -o ${.TARGET}

info::
	@echo -n '${INFO:@v@${v}="${${v}}"${.newline}@}'

# Combinable build targets
.ifmake(debug)
CXXFLAGS=  ${DBGFLAGS}
.endif
CXXFLAGS+= ${FLAGS}
.ifmake(paranoid)
CXXFLAGS+= ${PFLAGS}
.endif

debug paranoid: ${.TARGETS:Ndebug:Nparanoid:S/^$/all/W}

# Final addition to CXXFLAGS, target specific flags
CXXFLAGS+= ${CXXFLAGS.${.TARGET}}

# Build headers to verify they are consistent
headers: ${HPPS:T:=.pch}
.for h in ${HPPS}
${h:T:=.pch}: ${h}
	${CXX} ${CXXFLAGS} -c ${.ALLSRC} -o ${.TARGET}
.endfor

# Install
install: ${TARGETS}

install deinstall: pkg/${.TARGET:C,.*/,,}.sh pkg/files
	@${.CURDIR}/pkg/${.TARGET:C,.*/,,}.sh < ${.CURDIR}/pkg/files \
		DESTDIR="${DESTDIR}" PREFIX="${PREFIX}" DOCSDIR="${DOCSDIR}" \
		CURDIR="${.CURDIR}" OBJDIR="${.OBJDIR}"

# Clean
clean:
	rm -f ${CLEAN}

# Test release build and install
RTNAME=        ${.CURDIR:T:S,-*,,}-${VERSION}
RTMAKE=        env GIT_DIR=.nogit ${MAKE} -C${RTNAME} MAKEOBJDIR=
releasetest::
	@rm -rf ${RTNAME}
	@git clone ${.CURDIR} ${RTNAME}
	${RTMAKE} info
	${RTMAKE}
	${RTMAKE} install DESTDIR=root
	@rm -rf ${RTNAME}

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
	  ${MAKE} MAKEOBJDIR="${.OBJDIR}/${.TARGET}" CXX="${.TARGET:T}")
	@rmdir -p "${.TARGET}" 2> /dev/null ||:

# Build default target for all
testbuild: ${TESTBUILDS:S,^,testbuild/,}

# Build specific target
.for target in ${TBTARGETS}
${TESTBUILDS:S,^,testbuild/,:S,$,/${target},}::
	@mkdir -p "${.TARGET:H}"
	@echo [${.TARGET:H}]: ${MAKE} ${.TARGET:T} ${MAKEFLAGS:N.*}
	@(cd "${.CURDIR}" && \
	  ${MAKE} MAKEOBJDIR="${.OBJDIR}/${.TARGET:H}" CXX="${.TARGET:H:T}" \
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

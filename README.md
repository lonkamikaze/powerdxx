```
                                      /\  __
   __ __ ___  ___  __ _____  ___  ___/ /_/ /_
  /_//_//   \/   \/ // / _ \/ __\/    /_   _/__
 __ __ / // / // /    / ___/ /  / // /  /_/_/ /_
/_//_// ___/\___/\_/_/\___/_/   \___/     /_  _/
     / /                                   /_/
     \/ multi-core CPU clock daemon for FreeBSD®
```

The powerd++ daemon is a drop-in replacement for FreeBSD's native
powerd. Its purpose is to reduce the energy consumption of CPUs for
the following benefits:

- Avoid unnecessary fan noise from portable devices
- Improve the battery runtime of portable devices
- Improve hardware lifetime by reducing thermal stress
- Energy conservation

[code]:       https://github.com/lonkamikaze/powerdxx
[releases]:   https://github.com/lonkamikaze/powerdxx/releases
[issues]:     https://github.com/lonkamikaze/powerdxx/issues
[HTML]:       https://lonkamikaze.github.io/powerdxx
[PDF]:        https://lonkamikaze.github.io/powerdxx/refman.pdf

**Contents**

1. [Using powerd++](#using-powerd)
   1. [Packages](#packages)
   2. [Running powerd++](#running-powerd)
   3. [Manuals](#manuals)
   4. [Tuning](#tuning)
   5. [Reporting Issues / Requesting Features](#reporting-issues--requesting-features)
2. [Building/Installing](#buildinginstalling)
   1. [Building](#building)
   2. [Installing](#installing)
   3. [Documentation](#documentation)
3. [Development](#development)
   1. [Design](#design)
   2. [License](#license)

Using powerd++
==============

Powerd++ offers the following features:

- Load target based clock frequency control
- Tunable sampling with moving average filter
- Load recording and replay tooling for benchmarking, tuning and
  reporting issues
- Command line compatibility with [powerd(8)]
- Temperature based throttling
- Expressive command line arguments with units, ranges and argument
  chaining
- Helpful error messages
- Comprehensive manual pages

[powerd(8)]:  https://www.freebsd.org/cgi/man.cgi?query=powerd

Packages
--------

The [FreeBSD] port is `sysutils/powerdxx`, the package name `powerdxx`.

[FreeBSD]:    https://www.freebsd.org/

Running powerd++
----------------

It is not intended to run powerd++ simultaneously with powerd.
To prevent this powerd++ uses the same default pidfile as powerd:

```
# service powerdxx onestart
Starting powerdxx.
powerd++: (ECONFLICT) a power daemon is already running under PID: 59866
/usr/local/etc/rc.d/powerdxx: WARNING: failed to start powerdxx
```

So if powerd is already setup, it first needs to be disabled:

```
# service powerd stop
Stopping powerd.
Waiting for PIDS: 50127.
# service powerd disable
powerd disabled in /etc/rc.conf
```

Afterwards powerd++ can be enabled:

```
# service powerdxx enable
powerdxx enabled in /etc/rc.conf
# service powerdxx start
Starting powerdxx.
```

Manuals
-------

Comprehensive manual pages exist for powerd++ and its accompanying
tools loadrec and loadplay:

```
> man powerd++ loadrec loadplay
```

The current version of the manual pages may be read directly from
the repository:

```
> man man/*
```

The manual pages as of the last release can also be
[read online](https://lonkamikaze.github.io/powerdxx/pages.html).

Tuning
------

Three parameters affect the responsiveness of powerd++:

- The load target (refer to `-a`, `-b` and `-n`)
- The polling interval (refer to `-p`)
- The sample count (refer to `-s`)

The key to tuning powerd++ is the `-f` flag, which keeps powerd++
in foreground and causes it to report its activity.
This allows directly observing the effects of a parameter set.

Observing the defaults in action may be a good start:

```
# powerd++ -f
power:  online, load:  693 MHz,  42 C, cpu.0.freq: 2401 MHz, wanted: 1848 MHz
power:  online, load:  475 MHz,  43 C, cpu.0.freq: 1800 MHz, wanted: 1266 MHz
power:  online, load:  271 MHz,  43 C, cpu.0.freq: 1300 MHz, wanted:  722 MHz
power:  online, load:   64 MHz,  43 C, cpu.0.freq:  768 MHz, wanted:  170 MHz
power:  online, load:   55 MHz,  42 C, cpu.0.freq:  768 MHz, wanted:  146 MHz
power:  online, load:   57 MHz,  42 C, cpu.0.freq:  768 MHz, wanted:  152 MHz
power:  online, load:   60 MHz,  44 C, cpu.0.freq:  768 MHz, wanted:  160 MHz
power:  online, load:   67 MHz,  42 C, cpu.0.freq:  768 MHz, wanted:  178 MHz
...
```

Note, the immediate high load is due to the load buffer being filled
under the assumption that the past load fits the current clock frequency
when powerd++ starts.

Reporting Issues / Requesting Features
--------------------------------------

Please report issues and feature requests on [GitHub][issues] or
to <kamikaze@bsdforen.de>.

### Build Issues

In case of a build issue, please report the build output as well
as the output of `make info`:

```
> make info
VERSION="0.4.3+c8"
GITVERSION="0.4.3+c8"
GITHASH="8431d86abe7479a4c0a040c19551ff3fa2454ea1"
PKGVERSION=""
TARGETS="powerd++ loadrec loadplay libloadplay.so"
CXX="ccache c++"
CXXFLAGS="-O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic"
CXXVERSION="FreeBSD clang version 8.0.1 (tags/RELEASE_801/final 366581) (based on LLVM 8.0.1) Target
: x86_64-unknown-freebsd12.1 Thread model: posix InstalledDir: /usr/bin"
UNAME_A="FreeBSD AprilRyan.norad 12.1-STABLE FreeBSD 12.1-STABLE #1 ea071b9cb32(stable/12)-dirty: Mo
n Oct 28 23:37:31 CET 2019     root@AprilRyan.norad:/usr/obj/S403/amd64/usr/src/amd64.amd64/sys/S403
  amd64"
```

### Performance Issues

If powerd++ behaves in some unexpected or undesired manner, please
mention all the command line flags (e.g. from `/etc/rc.conf`
`powerdxx_flags`) and provide a load recording:

```
> loadrec -o myissue.load
```

The default recording duration is 30 s. Do not omit the `-o` parameter,
printing the output on the terminal may create significant load and
impact the recorded load significantly.

Before submitting the report, try to reproduce the behaviour using
the recorded load:

```
> loadplay -i myissue.load -o /dev/null powerd++ -f
power:  online, load:  224 MHz, cpu.0.freq:  768 MHz, wanted:  597 MHz
power:  online, load:  155 MHz, cpu.0.freq:  768 MHz, wanted:  413 MHz
power:  online, load:   85 MHz, cpu.0.freq:  768 MHz, wanted:  226 MHz
power:  online, load:   29 MHz, cpu.0.freq:  768 MHz, wanted:   77 MHz
power:  online, load:   23 MHz, cpu.0.freq:  768 MHz, wanted:   61 MHz
...
```

Building/Installing
===================

The `Makefile` offers a set of targets, it is written for FreeBSD's
[make(1)](https://www.freebsd.org/cgi/man.cgi?query=make):

| Target      | Description                                           |
|-------------|-------------------------------------------------------|
| all         | Build everything                                      |
| info        | Print the build configuration                         |
| debug       | Build with `CXXFLAGS=-O0 -g -DEBUG`                   |
| paranoid    | Turn on undefined behaviour canaries                  |
| install     | Install tools and manuals                             |
| deinstall   | Deinstall tools and manuals                           |
| clean       | Clear build directory `obj/`                          |
| releasetest | Attempt a build and install from a gitless repo clone |
| testbuild   | Test build with a set of compilers                    |
| tb          | Alias for testbuild                                   |
| doc         | Build HTML documentation                              |
| gh-pages    | Build and publish HTML and PDF documentation          |

Building
--------

The `all` target is the default target that is called implicitly if
make is run without arguments:

```
> make
c++  -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic -c src/powerd++.cpp -o powerd++.o
c++  -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic -c src/clas.cpp -o clas.o
c++ -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic powerd++.o clas.o -lutil -o powerd++
c++  -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic -c src/loadrec.cpp -o loadrec.o
c++ -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic loadrec.o clas.o -o loadrec
c++  -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic -c src/loadplay.cpp -o loadplay.o
c++ -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic loadplay.o clas.o -o loadplay
c++ -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic -fPIC -c src/libloadplay.cpp -o libloadplay.o
c++ -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic libloadplay.o -lpthread -shared -o libloadplay.so
>
```

The `debug` and `paranoid` flags perform the same build as the `all`
target, but with different/additional `CXXFLAGS`. The `debug` and
`paranoid` targets can be combined.

### `make testbuild` / `make tb`

The `testbuild` target builds all supported test builds, the list
of builds can be queried from the `TESTBUILDS` make variable:

```
> make -VTESTBUILDS
clang++90 clang++80 clang++70 g++9
```

A specific test build may be selected by appending it to the `testbuild` target:

```
> make tb/g++9
[testbuild/g++9]: make
g++9  -O2 -pipe -march=haswell  -std=c++17 -Wall -Werror -pedantic -c ../src/powerd++.cpp -o powerd++.o
...
```

Instead of creating the default target any non-documentation target
may be appended to the `testbuild` target:

```
> make tb/g++9/clean
[testbuild/g++9]: make clean
rm -f *.o powerd++ loadrec loadplay libloadplay.so
```

In order to run a specific target on all test builds, the build can
be omitted from the target:

```
> make tb/clean
[testbuild/clang++90]: make clean
rm -f *.o powerd++ loadrec loadplay libloadplay.so
[testbuild/clang++80]: make clean
rm -f *.o powerd++ loadrec loadplay libloadplay.so
[testbuild/clang++70]: make clean
rm -f *.o powerd++ loadrec loadplay libloadplay.so
[testbuild/g++9]: make clean
rm -f *.o powerd++ loadrec loadplay libloadplay.so
```

Installing
----------

The installer installs the tools and manual pages according to a recipe
in `pkg/files`. The following variables can be passed to `make install`
or `make deinstall` to affect the install destination:

| Variable  | Default                        |
|-----------|--------------------------------|
| `DESTDIR` |                                |
| `PREFIX`  | `/usr/local`                   |
| `DOCSDIR` | `${PREFIX}/share/doc/powerdxx` |

`DESTDIR` can be used to install powerd++ into a chroot or jail, e.g.
to put it into the staging area when building a package using the
FreeBSD ports. Unlike `PREFIX` and `DOCSDIR` it does not affect the
installed files themselves.

Documentation
-------------

Building the documentation requires `doxygen` 1.8.15 or later, building
the PDF version of the documentation requires `xelatex` as provided
by the tex-xetex package.

The `doc` target populates `doc/html` and `doc/latex`, to create the
PDF documentation `doc/latex/refman.pdf` must be built.

The `gh-pages` target builds the HTML and PDF documentation and drops
it into the `gh-pages` submodule for publishing on [github.io][HTML].

Development
===========

The following table provides an overview of repository contents:

| File/Folder   | Contents                                   |
|---------------|--------------------------------------------|
| `doc/`        | Output directory for doxygen documentation |
| `doxy/`       | Doxygen configuration and filter scripts   |
| `gh-pages/`   | Submodule for publishing the documentation |
| `man/`        | Manual pages written using mdoc(7) markup  |
| `obj/`        | Build output                               |
| `pkg/`        | Installer scripts and instructions         |
| `loads/`      | Load recordings useful for testing         |
| `src/`        | C++ source files                           |
| `src/sys/`    | C++ wrappers for common C interfaces       |
| `powerd++.rc` | Init script / service description          |
| `LICENSE.md`  | ISC license                                |
| `Makefile`    | Build instructions                         |
| `README.md`   | Project overview                           |

Design
------

The life cycle of the powerd++ process goes through three stages:

1. Command line argument parsing
2. Initialisation and optionally printing the detected/configured parameters
3. Clock frequency control

The first stage is designed to maximise usability by providing both,
the compact short option syntax (e.g. `-vfbhadp`) as well as the more
self-descriptive long option syntax
(e.g. `--verbose --foreground --batt hiadaptive`).

The second stage is designed to trigger all known error conditions
in order to fail before calling daemon(3) at the start of the third
stage. Both the first and second stage are meant to provide specific,
helpful error messages.

The third stage tracks the CPU load and performs clock frequency
control. It is designed to provide its functionality with as little
runtime as possible. This is achieved by:

- Using integer arithmetic only
- Minimising branching

The latter is achieved by using function templates to roll out possible
runtime state combinations as multiple functions. A single, central
switch/case selects the correct function each cycle. This basically
rolls out multiple code paths through a single function into multiple
functions with a single code path.

The trade-off made is for runtime over code size. With every bit
of state rolled out like this the number of functions that need to
be generated doubles, thus this approach is limited to the few bits
of state that control the most expensive functionality, e.g. the
foreground mode.

License
-------

This project is published under the [ISC license](LICENSE.md).

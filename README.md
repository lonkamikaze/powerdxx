	                                      /\  __
	   __ __ ___  ___  __ _____  ___  ___/ /_/ /_
	  /_//_//   \/   \/ // / _ \/ __\/    /_   _/__
	 __ __ / // / // /    / ___/ /  / // /  /_/_/ /_
	/_//_// ___/\___/\_/_/\___/_/   \___/     /_  _/
	     / /                                   /_/
	     \/ multi-core CPU clock daemon for FreeBSD®

powerd++
========

The `powerd++` daemon is a drop-in replacement for FreeBSD's native
`powerd(8)`. It monitors the system load and adjusts the CPU clock
accordingly, avoiding some of the pitfalls of `powerd`.

What Pitfalls?
--------------

At the time `powerd++` was first created (February 2016), `powerd`
exhibited some unhealthy behaviours on multi-core machines.

In order to make sure that single core loads do not suffer from the
use of `powerd` it was designed to use the sum load of all cores
as the current load rating. A side effect of this is that it causes
`powerd` to never clock down on systems with even moderate numbers
of cores. E.g. on a quad-core system with hyper threading a background
load of 12.5% per core suffices to score a 100% load rating.

The more cores are added, the worse it gets. Even on a dual core
machine (with HT) having a browser and an e-mail client open, suffices
to keep the load rating above 100% for most of the time, even without
user activity. Thus `powerd` never does its job of saving energy
by reducing the clock frequency.

Advantages of powerd++
----------------------

The `powerd++` implementation addresses this issue and more:

- `powerd++` groups cores with a common clock frequency together and
  handles each group's load and target frequency separately. I.e. the
  moment FreeBSD starts offering individual clock settings on the
  CPU, core or thread level, `powerd++` already supports it.
- `powerd++` takes the highest load within a group of cores to rate
  the load. This approach responds well to single core loads as well
  as evenly distributed loads.
- `powerd++` sets the clock frequency according to a load target, i.e.
  it jumps right to the clock rate it will stay in if the load does
  not change.
- `powerd++` supports taking the average load over more than two
  samples, this makes it more robust against small load spikes, but
  sacrifices less responsiveness than just increasing the polling
  interval would. Because only the oldest and the newest sample are
  required for calculating the average, this approach does not even
  cause additional runtime cost!
- `powerd++` parses command line arguments as floating point numbers,
  allowing expressive commands like `powerd++ --batt 1.2ghz`.

Building
--------

Download the repository and run `make`:

    > make
    c++ -O2 -pipe -Wall -Werror -pedantic -std=c++11 -Wall -Werror -pedantic -c src/powerd++.cpp -o powerd++.o
    c++ -lutil -O2 -pipe -Wall -Werror -pedantic -std=c++11 -Wall -Werror -pedantic -o powerd++ powerd++.o
    c++ -O2 -pipe -Wall -Werror -pedantic -std=c++11 -Wall -Werror -pedantic -c src/loadrec.cpp -o loadrec.o
    c++ -O2 -pipe -Wall -Werror -pedantic -std=c++11 -Wall -Werror -pedantic -o loadrec loadrec.o
    c++ -c -O2 -pipe -Wall -Werror -pedantic -std=c++11 -Wall -Werror -pedantic -fPIC -o loadplay.o src/loadplay.cpp
    c++ -lpthread -shared -O2 -pipe -Wall -Werror -pedantic -std=c++11 -Wall -Werror -pedantic -o libloadplay.so loadplay.o

Documentation
-------------

The manual pages can be read with the following commands:

    > man ./powerd++.8 ./loadrec.1 ./loadplay.1

Tooling
-------

In addition to the `powerd++` daemon this repository also comes with
the tools `loadrec` and `loadplay`. They can be used to record loads
and test both `powerd` and `powerd++` under reproducible load conditions.

This is great for tuning, testing, bug reports and creating fancy
plots.

FAQ
---

- **Why C++?** The `powerd++` code is not object oriented, but it uses
  some *C++* and *C++11* features to avoid common pitfalls of writing
  *C* code. E.g. there is a small *RAII* wrapper around the pidfile
  facilities (`pidfile_open()`, `pidfile_write()`, `pidfile_remove()`),
  turning the use of pidfiles into a fire and forget affair. Templated
  wrappers around calls like `sysctl()` use array references to infer
  buffer sizes at compile time, taking the burden of safely passing
  these buffer sizes on to the command away from the programmer.
  The `std::unique_ptr<>` template obsoletes memory cleanup code,
  providing the liberty of using exceptions without worrying about
  memory leaks.
- **Why does powerd++ show a high load when top shows a high idle time?**
  By default `top` shows the load percentage over all cores/threads,
  `powerd++` uses the load of a single core/thread (the one with the
  highest load). This keeps `powerd++` from starving single threaded
  processes, because they only have a small impact on overall load.
  An effect that increases with the number of cores/threads. E.g. 80%
  load on a quad core CPU with hyper threading only has an overall
  load impact of 10%. Use `top -P` to monitor idle times per core/thread.

LICENSE
-------

For those who care about this stuff, this project is available under
the [ISC license](LICENSE.md).


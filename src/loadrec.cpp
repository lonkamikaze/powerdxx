/** \file
 * Implements a load recorder, useful for simulating loads to test
 * CPU clock daemons and settings.
 */

#include "Options.hpp"

#include "types.hpp"
#include "constants.hpp"
#include "errors.hpp"
#include "utility.hpp"
#include "clas.hpp"
#include "fixme.hpp"

#include "sys/sysctl.hpp"

#include <iostream>  /* std::cout, std::cerr */
#include <fstream>   /* std::ofstream */
#include <chrono>    /* std::chrono::steady_clock::now() */
#include <thread>    /* std::this_thread::sleep_until() */
#include <memory>    /* std::unique_ptr */

#include <sys/resource.h>  /* CPUSTATES */

/**
 * File local scope.
 */
namespace {

using nih::Option;
using nih::make_Options;

using constants::POWERD_PIDFILE;
using constants::ACLINE;
using constants::FREQ;
using constants::FREQ_LEVELS;
using constants::CP_TIMES;

using types::ms;
using types::coreid_t;
using types::cptime_t;

using errors::Exit;
using errors::Exception;
using errors::fail;

using utility::to_value;
using utility::sprintf;
using namespace utility::literals;

using clas::ival;

using fixme::to_string;

using sys::ctl::make_Sysctl;
using sys::ctl::make_Once;

/**
 * The global state.
 */
struct {
	bool verbose{false};
	ms duration{30000};
	ms interval{25};
	std::ofstream outfile{};
	std::ostream * out = &std::cout;
	char const * outfilename{nullptr};
	sys::ctl::SysctlOnce<coreid_t, 2> const ncpu{0, {CTL_HW, HW_NCPU}};
} g;

/**
 * An enum for command line parsing.
 */
enum class OE {
	USAGE,           /**< Print help */
	IVAL_DURATION,   /**< Set the duration of the recording */
	IVAL_POLL,       /**< Set polling interval */
	FILE_OUTPUT,     /**< Set output file */
	FLAG_VERBOSE,    /**< Verbose output on stderr */
	OPT_UNKNOWN,     /**< Obligatory */
	OPT_NOOPT,       /**< Obligatory */
	OPT_DASH,        /**< Obligatory */
	OPT_LDASH,       /**< Obligatory */
	OPT_DONE         /**< Obligatory */
};

/**
 * The short usage string.
 */
char const * const USAGE = "[-hv] [-d ival] [-p ival] [-o file] [-P file]";

/**
 * Definitions of command line options.
 */
Option<OE> const OPTIONS[]{
	{OE::USAGE,         'h', "help",     "",     "Show usage and exit"},
	{OE::FLAG_VERBOSE,  'v', "verbose",  "",     "Be verbose"},
	{OE::IVAL_DURATION, 'd', "duration", "ival", "The duration of the recording"},
	{OE::IVAL_POLL,     'p', "poll",     "ival", "The polling interval"},
	{OE::FILE_OUTPUT,   'o', "output",   "file", "Output to file"},
};

/**
 * Outputs the given message on stderr if g.verbose is set.
 *
 * @param msg
 *	The message to output
 */
inline void verbose(std::string const & msg) {
	if (g.verbose) {
		std::cerr << "powerd++: " << msg << '\n';
	}
}

/**
 * Set up output to the given file.
 */
void init() {
	if (g.outfilename) {
		g.outfile.open(g.outfilename);
		if (!g.outfile.good()) {
			fail(Exit::EWOPEN, errno,
			     "could not open file for writing: "_s + g.outfilename);
		}
		g.out = &g.outfile;
	}
}

/**
 * Parse command line arguments.
 *
 * @param argc,argv
 *	The command line arguments
 */
void read_args(int const argc, char const * const argv[]) {
	auto getopt = make_Options(argc, argv, USAGE, OPTIONS);

	while (true) switch (getopt()) {
	case OE::USAGE:
		std::cerr << getopt.usage();
		throw Exception{Exit::OK, 0, ""};
	case OE::FLAG_VERBOSE:
		g.verbose = true;
		break;
	case OE::IVAL_DURATION:
		g.duration = ival(getopt[1]);
		break;
	case OE::IVAL_POLL:
		g.interval = ival(getopt[1]);
		break;
	case OE::FILE_OUTPUT:
		g.outfilename = getopt[1];
		break;
	case OE::OPT_UNKNOWN:
	case OE::OPT_NOOPT:
	case OE::OPT_DASH:
	case OE::OPT_LDASH:
		fail(Exit::ECLARG, 0, "unexpected command line argument: "_s +
		                      getopt[0] + "\n\n" + getopt.usage());
	case OE::OPT_DONE:
		return;
	}
}

/**
 * Print the sysctls
 */
void print_sysctls() {
	sys::ctl::Sysctl<3> hw_acpi_acline;
	try {
		hw_acpi_acline = {ACLINE};
	} catch (sys::sc_error<sys::ctl::error>) {
		verbose("cannot read "_s + ACLINE);
	}
	*g.out << "hw.machine=" << make_Sysctl(CTL_HW, HW_MACHINE).get<char>().get() << '\n'
	       << "hw.model=" << make_Sysctl(CTL_HW, HW_MODEL).get<char>().get() << '\n'
	       << "hw.ncpu=" << g.ncpu << '\n'
	       << ACLINE << '=' << make_Once(1U, hw_acpi_acline) << '\n';

	char mibname[40];
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		sprintf(mibname, FREQ, i);
		try {
			sys::ctl::Sysctl<4> ctl{mibname};
			*g.out << mibname << '='
			       << make_Once(0, ctl) << '\n';
		} catch (sys::sc_error<sys::ctl::error> e) {
			verbose("cannot access sysctl: "_s + mibname);
			if (i == 0) {
				fail(Exit::ENOFREQ, e,
				     "at least the first CPU core must support frequency updates");
			}
		}
		sprintf(mibname, FREQ_LEVELS, i);
		try {
			sys::ctl::Sysctl<4> ctl{mibname};
			*g.out << mibname << '='
			       << ctl.get<char>().get() << '\n';
		} catch (sys::sc_error<sys::ctl::error>) {
			/* do nada */
		}
	}
}

/**
 * Report the load frames.
 *
 * This prints the time in ms since the last frame and the cp_times
 * growth as a space separated list.
 */
void run() try {
	sys::ctl::Sysctl<2> const cp_times_ctl = {CP_TIMES};

	auto cp_times = std::unique_ptr<cptime_t[][CPUSTATES]>(
	    new cptime_t[2 * g.ncpu][CPUSTATES]{});

	auto time = std::chrono::steady_clock::now();
	auto last = time;
	auto const stop = time + g.duration;
	size_t sample = 0;
	/* Takes a sample and prints it, avoids duplicating code
	 * behind the loop. */
	auto const takeAndPrintSample = [&]() {
		cp_times_ctl.get(cp_times[sample * g.ncpu],
		                 g.ncpu * sizeof(cp_times[0]));
		*g.out << std::chrono::duration_cast<ms>(time - last).count();
		for (size_t i = 0; i < g.ncpu; ++i) {
			for (size_t q = 0; q < CPUSTATES; ++q) {
				*g.out << ' '
				       << (cp_times[sample * g.ncpu + i][q] -
				           cp_times[((sample + 1) % 2) * g.ncpu + i][q]);
			}
		}
	};
	while (time < stop) {
		takeAndPrintSample();
		*g.out << '\n';
		sample = (sample + 1) % 2;
		last = time;
		std::this_thread::sleep_until(time += g.interval);
	}
	takeAndPrintSample();
	*g.out << std::endl;
} catch (sys::sc_error<sys::ctl::error> e) {
	fail(Exit::ESYSCTL, e, "failed to access sysctl: "_s + CP_TIMES);
}

} /* namespace */


/**
 * Main routine, setup and execute daemon, print errors.
 *
 * @param argc,argv
 *	The command line arguments
 * @return
 *	An exit code
 * @see Exit
 */
int main(int argc, char * argv[]) {
	try {
		read_args(argc, argv);
		init();
		print_sysctls();
		run();
	} catch (Exception & e) {
		if (e.msg != "") {
			std::cerr << "loadrec: " << e.msg << '\n';
		}
		return to_value(e.exitcode);
	} catch (sys::sc_error<sys::ctl::error> e) {
		std::cerr << "loadrec: untreated sysctl failure: " << e.c_str() << '\n';
		throw;
	} catch (...) {
		std::cerr << "loadrec: untreated failure\n";
		throw;
	}
}

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

#include "sys/sysctl.hpp"

#include <iostream>  /* std::cout, std::cerr */
#include <fstream>   /* std::ofstream */
#include <chrono>    /* std::chrono::steady_clock::now() */
#include <thread>    /* std::this_thread::sleep_until() */
#include <memory>    /* std::unique_ptr */

#include <sys/resource.h>  /* CPUSTATES */
#include <sys/stat.h>      /* stat() */

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
using types::mhz_t;

using errors::Exit;
using errors::Exception;
using errors::fail;

using utility::to_value;
using utility::sprintf_safe;
using namespace utility::literals;

using clas::ival;

using sys::ctl::make_Sysctl;
using sys::ctl::make_Once;

/**
 * The global state.
 */
struct {
	bool verbose{false};  /**< Verbosity flag. */
	ms duration{30000};   /**< Recording duration in ms. */
	ms interval{25};      /**< Recording sample interval in ms. */

	/**
	 * The output file stream to use if an outfilename is provided
	 * on the CLI.
	 */
	std::ofstream outfile{};

	/**
	 * A pointer to the stream to use for output, either std::cout
	 * or outfile.
	 */
	std::ostream * out = &std::cout;

	/**
	 * The user provided output file name.
	 */
	char const * outfilename{nullptr};

	/**
	 * The PID file location for clock frequency daemons.
	 */
	char const * pidfilename{POWERD_PIDFILE};

	/**
	 * The number of CPU cores/threads.
	 */
	sys::ctl::SysctlOnce<coreid_t, 2> const ncpu{1U, {CTL_HW, HW_NCPU}};
} g;

/**
 * An enum for command line parsing.
 */
enum class OE {
	USAGE,           /**< Print help */
	IVAL_DURATION,   /**< Set the duration of the recording */
	IVAL_POLL,       /**< Set polling interval */
	FILE_OUTPUT,     /**< Set output file */
	FILE_PID,        /**< Set PID file */
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
char const * const USAGE = "[-hv] [-d ival] [-p ival] [-o file]";

/**
 * Definitions of command line options.
 */
Option<OE> const OPTIONS[]{
	{OE::USAGE,         'h', "help",     "",     "Show usage and exit"},
	{OE::FLAG_VERBOSE,  'v', "verbose",  "",     "Be verbose"},
	{OE::IVAL_DURATION, 'd', "duration", "ival", "The duration of the recording"},
	{OE::IVAL_POLL,     'p', "poll",     "ival", "The polling interval"},
	{OE::FILE_OUTPUT,   'o', "output",   "file", "Output to file"},
	{OE::FILE_PID,      'P', "pid",      "file", "PID file of the local clock frequency daemon"},
};

/**
 * Outputs the given message on stderr if g.verbose is set.
 *
 * @param msg
 *	The message to output
 */
inline void verbose(std::string const & msg) {
	if (g.verbose) {
		std::cerr << "loadrec: " << msg << '\n';
	}
}

/**
 * Set up output to the given file.
 */
void init() {
	for (struct stat dummy{}; 0 == stat(g.pidfilename, &dummy);) {
		fail(Exit::EPID, errno,
		     "please record without a clock control daemon running, pidfile encountered: "_s + POWERD_PIDFILE);
	}
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
	case OE::FILE_PID:
		g.pidfilename = getopt[1];
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
	sys::ctl::Sysctl<> hw_acpi_acline;
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
		sprintf_safe(mibname, FREQ, i);
		try {
			sys::ctl::Sysctl<> ctl{mibname};
			*g.out << mibname << '='
			       << make_Once(0, ctl) << '\n';
		} catch (sys::sc_error<sys::ctl::error> e) {
			verbose("cannot access sysctl: "_s + mibname);
			if (i == 0) {
				fail(Exit::ENOFREQ, e,
				     "at least the first CPU core must report its clock frequency");
			}
		}
		sprintf_safe(mibname, FREQ_LEVELS, i);
		try {
			sys::ctl::Sysctl<> ctl{mibname};
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
	/*
	 * Setup cptimes buffer for two samples.
	 */
	sys::ctl::Sysctl<> const cp_times_ctl = {CP_TIMES};

	auto const columns = cp_times_ctl.size() / sizeof(cptime_t);
	auto cp_times = std::unique_ptr<cptime_t[]>(
	    new cptime_t[2 * columns]{});

	/*
	 * Setup clock frequency sources for each core.
	 */
	coreid_t const cores = columns / CPUSTATES;
	auto corefreqs = std::unique_ptr<sys::ctl::Sync<mhz_t, sys::ctl::Sysctl<>>[]>(
	    new sys::ctl::Sync<mhz_t, sys::ctl::Sysctl<>>[cores]{});

	for (coreid_t i = 0; i < cores; ++i) {
		char mibname[40];
		sprintf_safe(mibname, FREQ, i);
		try {
			corefreqs[i] = sys::ctl::Sysctl<>{mibname};
		} catch (sys::sc_error<sys::ctl::error> e) {
			if (i == 0) {
				fail(Exit::ENOFREQ, e,
				     "at least the first CPU core must report its clock frequency");
			}
			/* Fall back to previous clock provider. */
			corefreqs[i] = corefreqs[i - 1];
		}
	}

	/*
	 * Record `cptimes * freq` in order to get an absolute measure of
	 * the load.
	 */
	auto time = std::chrono::steady_clock::now();
	auto last = time;
	auto const stop = time + g.duration;
	size_t sample = 0;
	/* Takes a sample and prints it, avoids duplicating code
	 * behind the loop. */
	auto const takeAndPrintSample = [&]() {
		cp_times_ctl.get(&cp_times[sample * columns],
		                 sizeof(cptime_t) * columns);
		*g.out << std::chrono::duration_cast<ms>(time - last).count();
		mhz_t freq = 1;
		for (int i = 0; i < columns; ++i) {
			if (i % CPUSTATES == 0) {
				freq = corefreqs[i / CPUSTATES];
			}
			*g.out << ' '
			       << freq * (cp_times[sample * columns + i] -
			                  cp_times[((sample + 1) % 2) * columns + i]);
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


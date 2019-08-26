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
#include "version.hpp"

#include "sys/io.hpp"
#include "sys/sysctl.hpp"

#include <chrono>    /* std::chrono::steady_clock::now() */
#include <thread>    /* std::this_thread::sleep_until() */
#include <memory>    /* std::unique_ptr */

#include <sys/resource.h>  /* CPUSTATES */

/**
 * File local scope.
 */
namespace {

using nih::Parameter;
using nih::make_Options;

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

namespace io = sys::io;

/**
 * Output file type alias.
 *
 * @tparam Ownership
 *	The io::ownership type of the file
 */
template <auto Ownership> using ofile = io::file<Ownership, io::write>;

using version::LOADREC_FEATURES;
using version::flag_t;
using namespace version::literals;

/**
 * The set of supported features.
 *
 * This value is stored in load recordings to allow loadplay to correctly
 * interpret the data.
 */
constexpr flag_t const FEATURES{
	1_FREQ_TRACKING
};

/**
 * The global state.
 */
struct {
	bool verbose{false};  /**< Verbosity flag. */
	ms duration{30000};   /**< Recording duration in ms. */
	ms interval{25};      /**< Recording sample interval in ms. */

	/**
	 * The output stream either io::fout (stdout) or a file.
	 */
	ofile<io::link> fout = io::fout;

	/**
	 * The user provided output file name.
	 */
	char const * outfilename{nullptr};

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
 * Definitions of command line parameters.
 */
Parameter<OE> const PARAMETERS[]{
	{OE::USAGE,         'h', "help",     "",     "Show usage and exit"},
	{OE::FLAG_VERBOSE,  'v', "verbose",  "",     "Be verbose"},
	{OE::IVAL_DURATION, 'd', "duration", "ival", "The duration of the recording"},
	{OE::IVAL_POLL,     'p', "poll",     "ival", "The polling interval"},
	{OE::FILE_OUTPUT,   'o', "output",   "file", "Output to file"},
	{OE::FILE_PID,      'P', "pid",      "file", "Ignored"},
};

/**
 * Outputs the given printf style message on stderr if g.verbose is set.
 *
 * @tparam MsgTs
 *	The message argument types
 * @param msg
 *	The message to output
 */
template <typename... MsgTs>
inline void verbose(MsgTs &&... msg) {
	if (g.verbose) {
		io::ferr.print("loadrec: ");
		io::ferr.printf(std::forward<MsgTs>(msg)...);
	}
}

/**
 * Set up output to the given file.
 */
void init() {
	if (g.outfilename) {
		static ofile<io::own> outfile{g.outfilename, "wb"};
		if (!outfile) {
			fail(Exit::EWOPEN, errno,
			     "could not open file for writing: "_s + g.outfilename);
		}
		g.fout = outfile;
	}
}

/**
 * Parse command line arguments.
 *
 * @param argc,argv
 *	The command line arguments
 */
void read_args(int const argc, char const * const argv[]) {
	auto getopt = make_Options(argc, argv, USAGE, PARAMETERS);

	try {
		while (true) switch (getopt()) {
		case OE::USAGE:
			io::ferr.printf("%s", getopt.usage().c_str());
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
			break;
		case OE::OPT_UNKNOWN:
		case OE::OPT_NOOPT:
		case OE::OPT_DASH:
		case OE::OPT_LDASH:
			fail(Exit::ECLARG, 0,
			     "unexpected command line argument: "_s + getopt[0]);
		case OE::OPT_DONE:
			return;
		}
	} catch (Exception & e) {
		switch (getopt) {
		case OE::USAGE:
			break;
		case OE::FLAG_VERBOSE:
			e.msg += "\n\n";
			e.msg += getopt.show(0);
			break;
		case OE::IVAL_DURATION:
		case OE::IVAL_POLL:
		case OE::FILE_OUTPUT:
		case OE::FILE_PID:
			e.msg += "\n\n";
			e.msg += getopt.show(1);
			break;
		case OE::OPT_UNKNOWN:
		case OE::OPT_NOOPT:
		case OE::OPT_DASH:
		case OE::OPT_LDASH:
			e.msg += "\n\n";
			e.msg += getopt.show(0);
			e.msg += "\n\n";
			e.msg += getopt.usage();
			break;
		case OE::OPT_DONE:
			return;
		}
		throw;
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
		verbose("cannot read %s\n", ACLINE);
	}
	g.fout.printf("%s=%ld\n"
	              "hw.machine=%s\n"
	              "hw.model=%s\n"
	              "hw.ncpu=%d\n"
	              "%s=%d\n",
	              LOADREC_FEATURES, FEATURES,
	              make_Sysctl(CTL_HW, HW_MACHINE).get<char>().get(),
	              make_Sysctl(CTL_HW, HW_MODEL).get<char>().get(),
	              g.ncpu,
	              ACLINE, make_Once(1U, hw_acpi_acline));

	for (coreid_t i = 0; i < g.ncpu; ++i) {
		char mibname[40];
		sprintf_safe(mibname, FREQ, i);
		try {
			sys::ctl::Sysctl<> ctl{mibname};
			g.fout.printf("%s=%d\n", mibname, make_Once(0, ctl));
		} catch (sys::sc_error<sys::ctl::error> e) {
			verbose("cannot access sysctl: %s\n", mibname);
			if (i == 0) {
				fail(Exit::ENOFREQ, e,
				     "at least the first CPU core must report its clock frequency");
			}
		}
		sprintf_safe(mibname, FREQ_LEVELS, i);
		try {
			sys::ctl::Sysctl<> ctl{mibname};
			g.fout.printf("%s=%s\n", mibname, ctl.get<char>().get());
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
	 * Record freq and cptimes.
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
		g.fout.printf("%lld", std::chrono::duration_cast<ms>(time - last).count());
		for (coreid_t i = 0; i < cores; ++i) {
			g.fout.printf(" %u", static_cast<mhz_t>(corefreqs[i]));
		}
		for (size_t i = 0; i < columns; ++i) {
			g.fout.printf(" %lu", cp_times[sample * columns + i] -
			                      cp_times[((sample + 1) % 2) * columns + i]);
		}
		g.fout.putc('\n');
	};
	while (time < stop) {
		takeAndPrintSample();
		sample = (sample + 1) % 2;
		last = time;
		std::this_thread::sleep_until(time += g.interval);
	}
	takeAndPrintSample();
	g.fout.flush();
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
int main(int argc, char * argv[]) try {
	read_args(argc, argv);
	init();
	print_sysctls();
	run();
	return to_value(Exit::OK);
} catch (Exception & e) {
	if (e.msg != "") {
		io::ferr.printf("loadrec: %s\n", e.msg.c_str());
	}
	return to_value(e.exitcode);
} catch (sys::sc_error<sys::ctl::error> e) {
	io::ferr.printf("loadrec: untreated sysctl failure: %s\n", e.c_str());
	return to_value(Exit::EEXCEPT);
} catch (...) {
	io::ferr.print("loadrec: untreated failure\n");
	return to_value(Exit::EEXCEPT);
}

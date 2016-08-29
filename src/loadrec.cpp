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

namespace {

using nih::Option;
using nih::make_Options;

using constants::POWERD_PIDFILE;

using types::ms;

using errors::Exit;
using errors::Exception;
using errors::fail;

using utility::to_value;
using namespace utility::literals;

using clas::ival;

/**
 * The global state.
 */
struct {
	bool verbose{false};
	ms duration{0};
	ms interval{25};
	std::ofstream outfile{};
	std::ostream * out = &std::cout;
	char const * pidfilename = POWERD_PIDFILE;
} g;

/**
 * An enum for command line parsing.
 */
enum class OE {
	USAGE,           /**< Print help */
	IVAL_DURATION,   /**< Set the duration of the recording */
	IVAL_POLL,       /**< Set polling interval */
	FILE_OUTPUT,     /**< Set output file */
	FILE_PID,        /**< Set pidfile */
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
	{OE::FILE_PID,      'P', "pid",      "file", "Alternative PID file"}
};

/**
 * Set up output to the given file.
 *
 * @param name
 *	Name of the file
 */
void set_outfile(char const * const name) {
	g.outfile.open(name);
	if (!g.outfile.good()) {
		fail(Exit::EWOPEN, errno,
		     "could not open file for writing: "_s + name);
	}
	g.out = &g.outfile;
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
		set_outfile(getopt[1]);
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


using namespace types;
using namespace constants;

} /* namespace */

int main(int argc, char * argv[]) {
	try {
		read_args(argc, argv);
	} catch (Exception & e) {
		if (e.msg != "") {
			std::cerr << "loadrec: " << e.msg << '\n';
		}
		return to_value(e.exitcode);
	}
	sys::ctl::SysctlOnce<coreid_t, 2> const ncpu{0, {CTL_HW, HW_NCPU}};
	sys::ctl::Sysctl<2> const cp_times_ctl = {CP_TIMES};

	auto cp_times = std::unique_ptr<cptime_t[][CPUSTATES]>(
	    new cptime_t[2 * ncpu][CPUSTATES]{});

	auto time = std::chrono::steady_clock::now();
	//auto const start = time;
	size_t sample = 0;
	while (true) {
		std::this_thread::sleep_until(time += ms{50});
		cp_times_ctl.get(cp_times[sample * ncpu],
		                 ncpu * sizeof(cp_times[0]));
		for (size_t i = 0; i < ncpu; ++i) {
			std::cout << "cpu" << i << ':';
			for (size_t q = 0; q < CPUSTATES; ++q) {
				std::cout << ' '
				          << (cp_times[sample * ncpu + i][q] -
				              cp_times[((sample + 1) % 2) * ncpu + i][q]);
			}
			std::cout << '\n';
		}
		sample = (sample + 1) % 2;
	}
}


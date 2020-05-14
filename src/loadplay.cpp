/**
 * Implements loadplay, a bootstrapping tool for libloadplay.
 *
 * @file
 */

#include "Options.hpp"

#include "errors.hpp"
#include "utility.hpp"

#include "sys/env.hpp"
#include "sys/io.hpp"

#include <unistd.h>  /* execvp() */

/**
 * File local scope.
 */
namespace {

using nih::Parameter;
using nih::Options;

using errors::Exit;
using errors::Exception;
using errors::fail;

namespace io = sys::io;

using utility::to_value;
using namespace utility::literals;

using namespace std::literals::string_literals;

/**
 * An enum for command line parsing.
 */
enum class OE {
	USAGE,            /**< Print help */
	FILE_IN,          /**< Set input file instead of stdin */
	FILE_OUT,         /**< Set output file instead of stdout */
	CMD,              /**< The command to execute */
	OPT_NOOPT = CMD,  /**< Obligatory */
	OPT_UNKNOWN,      /**< Obligatory */
	OPT_DASH,         /**< Obligatory */
	OPT_LDASH,        /**< Obligatory */
	OPT_DONE          /**< Obligatory */
};

/**
 * The short usage string.
 */
char const * const USAGE = "[-h] [-i file] [-o file] command [...]";

/**
 * Definitions of command line parameters.
 */
Parameter<OE> const PARAMETERS[]{
	{OE::USAGE,    'h', "help",   "",              "Show usage and exit"},
	{OE::FILE_IN,  'i', "input",  "file",          "Input file (load recording)"},
	{OE::FILE_OUT, 'o', "output", "file",          "Output file (replay stats)"},
	{OE::CMD,       0 , "",       "command,[...]", "The command to execute"},
};

/**
 * Performs very rudimentary file name argument checks.
 *
 * - Fail on empty path
 * - Return nullptr on '-'
 *
 * @param path
 *	The file path to check
 * @return
 *	The given path or nullptr if the given path is '-'
 */
char const * filename(char const * const path) {
	if (!path || !path[0]) {
		fail(Exit::EFILE, 0, "empty or missing string for filename");
	}
	if ("-"s == path) {
		return nullptr;
	}
	return path;
}

/**
 * Executes the given command, substituting this process.
 *
 * This function is a wrapper around execvp(3) and does not return.
 *
 * @param file
 *	The command to execute, looked up in PATH if no path is provided
 * @param argv
 *	The command line arguments of the command
 * @throws errors::Exception{Exit::EEXEC}
 */
[[noreturn]] void execute(char const * const file, char * const argv[]) {
	if (!file || !file[0]) {
		fail(Exit::EEXEC, 0, "failed to execute empty command");
	}
	execvp(file, argv);
	/* ^^ must not return */
	fail(Exit::EEXEC, errno,
	     "failed to execute %s: %s"_fmt(file, strerror(errno)));
}

/**
 * If running from an explicit path add the path to the library search path.
 *
 * This function facilitates calling `loadplay` directly from the build
 * directory for testing and allows it to pick up `libloadplay.so` from
 * the same directory.
 *
 * @param argc,argv
 *	The command line arguments provided to loadplay
 * @pre argc >= 2
 * @warning
 *	This function changes the contents of argv[0]
 */
void set_library_path(int const argc, char * const argv[]) {
	assert(argc >= 2);
	/* search argv[0] for a / from right to left */
	auto argp = argv[1] - 1;
	while (--argp >= argv[0] && *argp != '/');

	/* replace the / with a cstring terminator and set LD_LIBRARY_PATH
	 * to the resulting path in argv[0] */
	if (argp >= argv[0]) {
		*argp = 0;
		sys::env::vars["LD_LIBRARY_PATH"] = argv[0];
		return;
	}
}

} /* namespace */

/**
 * Parse command line arguments and execute the given command.
 *
 * @param argc,argv
 *	The command line arguments
 * @return
 *	An exit code
 * @see Exit
 */
int main(int argc, char * argv[]) try {
	auto & env = sys::env::vars;

	auto getopt = Options{argc, argv, USAGE, PARAMETERS};

	try {
		while (true) switch (getopt()) {
		case OE::USAGE:
			io::ferr.printf("%s", getopt.usage().c_str());
			throw Exception{Exit::OK, 0, ""};
		case OE::FILE_IN:
			env["LOADPLAY_IN"] = filename(getopt[1]);
			break;
		case OE::FILE_OUT:
			env["LOADPLAY_OUT"] = filename(getopt[1]);
			break;
		case OE::CMD:
			env["LD_PRELOAD"] = "libloadplay.so";
			assert(getopt.offset() < argc &&
			       "only OPT_DONE may violate this constraint");
			set_library_path(argc, argv);
			/* forward the remainder of the arguments */
			execute(getopt[0], argv + getopt.offset());
			break;
		case OE::OPT_UNKNOWN:
		case OE::OPT_DASH:
		case OE::OPT_LDASH:
			fail(Exit::ECLARG, 0,
			     "unexpected command line argument: "s + getopt[0]);
			break;
		case OE::OPT_DONE:
			fail(Exit::ECLARG, 0, "command expected");
			break;
		}
	} catch (Exception & e) {
		switch (getopt) {
		case OE::USAGE:
			break;
		case OE::FILE_IN:
		case OE::FILE_OUT:
			e.msg += "\n\n"s += getopt.show(1);
			break;
		case OE::CMD:
			e.msg += "\n\n"s += getopt.show(0, 0);
			break;
		case OE::OPT_UNKNOWN:
		case OE::OPT_DASH:
		case OE::OPT_LDASH:
		case OE::OPT_DONE:
			e.msg += "\n\n"s += getopt.show(0);
			break;
		}
		throw;
	}
	assert(!"must never be reached");
} catch (Exception & e) {
	if (e.msg != "") {
		io::ferr.printf("loadplay: %s\n", e.msg.c_str());
	}
	return to_value(e.exitcode);
} catch (...) {
	io::ferr.print("loadplay: untreated failure\n");
	return to_value(Exit::EEXCEPT);
}

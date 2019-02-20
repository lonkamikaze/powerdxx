/** \file
 * Implements the loadplay a bootstrapping tool for libloadplay.
 */

#include "Options.hpp"

#include "errors.hpp"
#include "utility.hpp"

#include "sys/env.hpp"

#include <iostream>  /* std::cerr */

#include <unistd.h>  /* execvp() */

/**
 * File local scope.
 */
namespace {

using nih::Parameter;
using nih::make_Options;

using errors::Exit;
using errors::Exception;
using errors::fail;

using utility::to_value;
using namespace utility::literals;

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
	if ("-"_s == path) {
		return nullptr;
	}
	return path;
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

	auto getopt = make_Options(argc, argv, USAGE, PARAMETERS);

	try {
		while (true) switch (getopt()) {
		case OE::USAGE:
			std::cerr << getopt.usage();
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
			/* forward the remainder of the arguments */
			execvp(getopt[0], argv + getopt.offset());
			/* ^^ must not return */
			fail(Exit::EEXEC, errno,
			     "failed to execute %s: %s"_fmt
			     (getopt[0], strerror(errno)));
			break;
		case OE::OPT_UNKNOWN:
		case OE::OPT_DASH:
		case OE::OPT_LDASH:
			fail(Exit::ECLARG, 0,
			     "unexpected command line argument: "_s + getopt[0]);
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
			e.msg += "\n\n"_s += getopt.show(1);
			break;
		case OE::CMD:
			e.msg += "\n\n"_s += getopt.show(0, 0);
			break;
		case OE::OPT_UNKNOWN:
		case OE::OPT_DASH:
		case OE::OPT_LDASH:
		case OE::OPT_DONE:
			e.msg += "\n\n"_s += getopt.show(0) += "\n\n"_s
			      += getopt.usage();
			break;
		}
		throw;
	}
	assert(!"must never be reached");
} catch (Exception & e) {
	if (e.msg != "") {
		std::cerr << "loadplay: " << e.msg << '\n';
	}
	return to_value(e.exitcode);
} catch (...) {
	std::cerr << "loadplay: untreated failure\n";
	return to_value(Exit::EEXCEPT);
}

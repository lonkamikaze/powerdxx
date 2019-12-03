/**
 * Common error handling code.
 *
 * @file
 */

#include "utility.hpp"

#ifndef _POWERDXX_ERRORS_HPP_
#define _POWERDXX_ERRORS_HPP_

/**
 * Common error handling types and functions.
 */
namespace errors {

using namespace std::literals::string_literals;

/**
 * Exit codes.
 */
enum class Exit : int {
	OK,           /**< Regular termination */
	ECLARG,       /**< Unexpected command line argument */
	EOUTOFRANGE,  /**< A user provided value is out of range */
	ELOAD,        /**< The provided value is not a valid load */
	EFREQ,        /**< The provided value is not a valid frequency */
	EMODE,        /**< The provided value is not a valid mode */
	EIVAL,        /**< The provided value is not a valid interval */
	ESAMPLES,     /**< The provided value is not a valid sample count */
	ESYSCTL,      /**< A sysctl operation failed */
	ENOFREQ,      /**< System does not support changing core frequencies */
	ECONFLICT,    /**< Another frequency daemon instance is running */
	EPID,         /**< A pidfile could not be created */
	EFORBIDDEN,   /**< Insufficient privileges to change sysctl */
	EDAEMON,      /**< Unable to detach from terminal */
	EWOPEN,       /**< Could not open file for writing */
	ESIGNAL,      /**< Failed to install signal handler */
	ERANGEFMT,    /**< A user provided range is missing the separator */
	ETEMPERATURE, /**< The provided value is not a valid temperature */
	EEXCEPT,      /**< Untreated exception */
	EFILE,        /**< Not a valid file name */
	EEXEC,        /**< Command execution failed */
	LENGTH        /**< Enum length */
};

/**
 * Printable strings for exit codes.
 */
const char * const ExitStr[]{
	"OK", "ECLARG", "EOUTOFRANGE", "ELOAD", "EFREQ", "EMODE", "EIVAL",
	"ESAMPLES", "ESYSCTL", "ENOFREQ", "ECONFLICT", "EPID", "EFORBIDDEN",
	"EDAEMON", "EWOPEN", "ESIGNAL", "ERANGEFMT", "ETEMPERATURE",
	"EEXCEPT", "EFILE", "EEXEC"
};

static_assert(size_t{utility::to_value(Exit::LENGTH)} == utility::countof(ExitStr),
              "Every Exit code must have a string representation");

/**
 * Exceptions bundle an exit code, errno value and message.
 */
struct Exception {
	/**
	 * The code to exit with.
	 */
	Exit exitcode;

	/**
	 * The errno value at the time of creation.
	 */
	int err;

	/**
	 * An error message.
	 */
	std::string msg;
};

/**
 * Throws an Exception instance with the given message.
 *
 * @param exitcode
 *	The exit code to return on termination
 * @param err
 *	The errno value at the time the exception was created
 * @param msg
 *	The message to show
 */
[[noreturn]] inline void
fail(Exit const exitcode, int const err, std::string const & msg) {
	throw Exception{exitcode, err, "("s +
	                               ExitStr[utility::to_value(exitcode)] +
	                               ") " + msg};
}

} /* namespace errors */

#endif /* _POWERDXX_ERRORS_HPP_ */

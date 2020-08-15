/**
 * Provides functions to process command line arguments.
 *
 * @file
 */

#ifndef _POWERDXX_CLAS_HPP_
#define _POWERDXX_CLAS_HPP_

#include "types.hpp"
#include "errors.hpp"
#include "utility.hpp"

#include <string>    /* std::string */
#include <utility>   /* std::pair() */

/**
 * A collection of functions to process command line arguments.
 */
namespace clas {

using namespace std::literals::string_literals;

/**
 * Convert string to load in the range [0, 1024].
 *
 * The given string must have the following format:
 *
 * \verbatim
 * load = <float>, [ "%" ];
 * \endverbatim
 *
 * The input value must be in the range [0.0, 1.0] or [0%, 100%].
 *
 * @param str
 *	A string encoded load
 * @retval [0, 1024]
 *	The load given by str
 * @retval > 1024
 *	The given string is not a load
 */
types::cptime_t load(char const * const str);

/**
 * Convert string to frequency in MHz.
 *
 * The given string must have the following format:
 *
 * \verbatim
 * freq = <float>, [ "hz" | "khz" | "mhz" | "ghz" | "thz" ];
 * \endverbatim
 *
 * For compatibility with powerd MHz are assumed, if no unit string is given.
 *
 * The resulting frequency must be in the range [0Hz, 1THz].
 *
 * @param str
 *	A string encoded frequency
 * @return
 *	The frequency given by str
 */
types::mhz_t freq(char const * const str);

/**
 * Convert string to time interval in milliseconds.
 *
 * The given string must have the following format:
 *
 * \verbatim
 * ival = <float>, [ "s" | "ms" ];
 * \endverbatim
 *
 * For compatibility with powerd scalar values are assumed to represent
 * milliseconds.
 *
 * @param str
 *	A string encoded time interval
 * @return
 *	The interval in milliseconds
 */
types::ms ival(char const * const str);

/**
 * A string encoded number of samples.
 *
 * The string is expected to contain a scalar integer.
 *
 * @param str
 *	The string containing the number of samples
 * @return
 *	The number of samples
 */
size_t samples(char const * const str);

/**
 * Convert string to temperature in dK.
 *
 * The given string must have the following format:
 *
 * \verbatim
 * temperature = <float>, [ "C" | "K" | "F" | "R" ];
 * \endverbatim
 *
 * In absence of a unit °C is assumed.
 *
 * @param str
 *	A string encoded temperature
 * @return
 *	The temperature given by str
 */
types::decikelvin_t temperature(char const * const str);

/**
 * Converts dK into °C for display purposes.
 *
 * @param val
 *	A temperature in dK
 * @return
 *	The temperature in °C
 */
inline int celsius(types::decikelvin_t const val) {
	auto const valc = val - 2731;
	return (valc + (valc >= 0) * 5 - (valc < 0) * 5) / 10;
}

/**
 * Takes a string encoded range of values and returns them.
 *
 * A range has the format `from:to`.
 *
 * @tparam T
 *	The return type of the conversion function
 * @param func
 *	The function that converts the values from the string
 * @param str
 *	The string containing the range
 * @return
 *	A pair with the `from` and `to` values
 */
template <typename T>
std::pair<T, T> range(T (& func)(char const * const), char const * const str) {
	std::pair<T, T> result;
	std::string first{str};

	if (first == "") {
		/* give func() an opportunity to fail */
		func(str);
		errors::fail(errors::Exit::ERANGEFMT, 0, "range missing");
	}

	auto const sep = first.find(':');
	if (sep == std::string::npos) {
		errors::fail(errors::Exit::ERANGEFMT, 0,
		             "missing colon separator in range: "s +=
		             utility::sanitise(str));
	}
	first.erase(sep);
	result.first = func(first.c_str());
	result.second = func(str + sep + 1);
	return result;
}

/**
 * Verify that the given string only contains characters allowed in
 * sysctl names.
 *
 * The currently permitted characters are: `[0-9A-Za-z%._-]`
 *
 * @throws errors::Exit::ESYSCTLNAME
 *	For empty or invalid strings
 * @return
 *	The given string
 */
char const * sysctlname(char const * const str);

/**
 * Sanitise user-provided formatting strings.
 *
 * Ensure that the given string contains no more than the given formatting
 * fields in the given order.
 *
 * This only passes plain data format fields, no flags, field width
 * or precision are allowed.
 *
 * @throws errors::Exit::EFORMATFIELD
 *	For unexpected formatting fields
 * @param fmt
 *	The formatting string to sanitise
 * @param fields
 *	A set of characters representing a printf-style formatting
 * @return
 *	The given string
 */
template <typename ... CharTs>
char const * formatfields(char const * const fmt, CharTs const ... fields) {
	using namespace utility::literals;
	using utility::highlight;
	using errors::fail;
	using errors::Exit;

	auto it = fmt;
	for (char const expect : {fields ..., '%'}) {
		for (; it && *it; ++it) {
			if (it[0] == '%' && it[1] == '%') {
				++it;
				continue;
			}
			if (it[0] == '%' && it[1] == expect) {
				++it;
				break;
			}
			if (it[0] == '%') {
				auto const hl = highlight(fmt, it - fmt, 2);
				fail(Exit::EFORMATFIELD, 0,
				     "unexpected formatting field: "s +
				     "\n\t" + hl.text + "\n\t" + hl.line);
			}
		}
	}
	return fmt;
}

} /* namespace clas */

#endif /* _POWERDXX_CLAS_HPP_ */

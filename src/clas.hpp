/** \file
 * Implements functions to process command line arguments.
 */

#ifndef _POWERDXX_CLAS_HPP_
#define _POWERDXX_CLAS_HPP_

#include "types.hpp"
#include "errors.hpp"
#include "utility.hpp"

#include <cstdlib>   /* atof() */

#include <string>    /* std::string */
#include <locale>    /* std::tolower() */
#include <utility>   /* std::pair() */

/**
 * A collection of functions to process command line arguments.
 */
namespace clas {

using namespace utility::literals;

/**
 * Command line argument units.
 *
 * These units are supported for command line arguments, for SCALAR
 * arguments the behaviour of powerd is to be imitated.
 */
enum class Unit : size_t {
	SCALAR,      /**< Values without a unit */
	PERCENT,     /**< % */
	SECOND,      /**< s */
	MILLISECOND, /**< ms */
	HZ,          /**< hz */
	KHZ,         /**< khz */
	MHZ,         /**< mhz */
	GHZ,         /**< ghz */
	THZ,         /**< thz */
	CELSIUS,     /**< C */
	KELVIN,      /**< K */
	FAHRENHEIT,  /**< F */
	RANKINE,     /**< R */
	UNKNOWN      /**< Unknown unit */
};

/**
 * The unit strings on the command line, for the respective Unit instances.
 */
char const * const UnitStr[]{
	"", "%", "s", "ms", "hz", "khz", "mhz", "ghz", "thz", "C", "K", "F", "R"
};

/**
 * Determine the unit of a string encoded value.
 *
 * @param str
 *	The string to determine the unit of
 * @return
 *	A unit
 */
Unit unit(std::string const & str) {
	size_t pos = str[0] == '+' || str[0] == '-' ? 1 : 0;
	for (; pos < str.length() && ((str[pos] >= '0' && str[pos] <= '9') ||
	                              str[pos] == '.'); ++pos);
	auto const unitstr = str.substr(pos);
	for (size_t i = 0; i < utility::countof(UnitStr); ++i) {
		if (unitstr == UnitStr[i]) {
			return static_cast<Unit>(i);
		}
	}
	return Unit::UNKNOWN;
}

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
types::cptime_t load(char const * const str) {
	std::string load{str};
	for (char & ch : load) { ch = std::tolower(ch); }

	auto value = atof(str);
	switch (unit(load)) {
	case Unit::SCALAR:
		if (value > 1. || value < 0) {
			errors::fail(errors::Exit::EOUTOFRANGE, 0,
			             "load targets must be in the range [0.0, 1.0]: "_s + str);
		}
		/* convert load to [0, 1024] range */
		value = 1024 * value;
		return value < 1 ? 1 : value;
	case Unit::PERCENT:
		if (value > 100. || value < 0) {
			errors::fail(errors::Exit::EOUTOFRANGE, 0,
			             "load targets must be in the range [0%, 100%]: "_s + str);
		}
		/* convert load to [0, 1024] range */
		value = 1024 * (value / 100.);
		return value < 1 ? 1 : value;
	default:
		break;
	}
	errors::fail(errors::Exit::ELOAD, 0, "load target not recognised: "_s + str);
}

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
types::mhz_t freq(char const * const str) {
	std::string freqstr{str};
	for (char & ch : freqstr) { ch = std::tolower(ch); }

	auto value = atof(str);
	switch (unit(freqstr)) {
	case Unit::HZ:
		value /= 1000000.;
		break;
	case Unit::KHZ:
		value /= 1000.;
		break;
	case Unit::SCALAR: /* for compatibilty with powerd */
	case Unit::MHZ:
		break;
	case Unit::GHZ:
		value *= 1000.;
		break;
	case Unit::THZ:
		value *= 1000000.;
		break;
	default:
		errors::fail(errors::Exit::EFREQ, 0,
		             "frequency value not recognised: "_s + str);
	}
	if (value > 1000000. || value < 0) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "target frequency must be in the range [0Hz, 1THz]: "_s + str);
	}
	return types::mhz_t(value);
}

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
types::ms ival(char const * const str) {
	std::string interval{str};
	for (char & ch : interval) { ch = std::tolower(ch); }

	auto value = atof(str);
	if (value < 0) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		     "interval must be positive: "_s + str);
	}
	switch (unit(interval)) {
	case Unit::SECOND:
		return types::ms{static_cast<long long>(value * 1000.)};
	case Unit::SCALAR: /* for powerd compatibility */
	case Unit::MILLISECOND:
		return types::ms{static_cast<long long>(value)};
	default:
		break;
	}
	errors::fail(errors::Exit::EIVAL, 0,
	             "interval not recognised: "_s + str);
}

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
size_t samples(char const * const str) {
	if (unit(str) != Unit::SCALAR) {
		errors::fail(errors::Exit::ESAMPLES, 0,
		             "sample count must be a scalar integer: "_s + str);
	}
	auto const cnt = atoi(str);
	auto const cntf = atof(str);
	if (cntf != cnt) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "sample count must be an integer: "_s + str);
	}
	if (cnt < 1 || cnt > 1000) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "sample count must be in the range [1, 1000]: "_s + str);
	}
	return size_t(cnt);
}

/**
 * Convert string to temperature in °C.
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
types::celsius_t temperature(char const * const str) {
	std::string tempstr{str};
	for (char & ch : tempstr) { ch = std::toupper(ch); }

	auto value = atof(str);
	switch (unit(tempstr)) {
	case Unit::SCALAR:
	case Unit::CELSIUS:
		break;
	case Unit::KELVIN:
		value -= 273.15;
		break;
	case Unit::FAHRENHEIT:
		value = (value - 32.) * 5. / 9.;
		break;
	case Unit::RANKINE:
		value = (value - 491.67) * 5. / 9.;
		break;
	default:
		errors::fail(errors::Exit::ETEMPERATURE, 0,
		             "temperature value not recognised: "_s + str);
	}
	if (value < - 273.15) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "temperature must be above absolute zero (-273.15°C): "_s + str);
	}
	return types::celsius_t(value);
}

/**
 * Takes a string encoded range of values and returns them.
 *
 * A range has the format `from:to`.
 *
 * @tparam T
 *	The return type of the conversion function
 * @parma str
 *	The string containing the range
 * @param func
 *	The function that converts the values from the string
 * @return
 *	A pair with the `from` and `to` values
 */
template <typename T>
std::pair<T, T> range(char const * const str, T (* func)(char const * const)) {
	std::pair<T, T> result;
	std::string first{str};
	auto const sep = first.find(':');
	if (sep == std::string::npos) {
		errors::fail(errors::Exit::ERANGEFMT, 0,
		             "missing colon separator in range: "_s + str);
	}
	first.erase(sep);
	result.first = func(first.c_str());
	result.second = func(str + sep + 1);
	return result;
}

} /* namespace clas */

#endif /* _POWERDXX_CLAS_HPP_ */

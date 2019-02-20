#include "clas.hpp"

#include <cstdlib>   /* strtod(), strtol() */

#include <locale>    /* std::tolower() */

/**
 * File local scope.
 */
namespace {
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
} /* namespace */

types::cptime_t clas::load(char const * const str) {
	std::string load{str};
	for (char & ch : load) { ch = std::tolower(ch); }

	if (load == "") {
		errors::fail(errors::Exit::ELOAD, 0,
		             "load target value missing");
	}

	auto value = strtod(str, nullptr);
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

types::mhz_t clas::freq(char const * const str) {
	std::string freqstr{str};
	for (char & ch : freqstr) { ch = std::tolower(ch); }

	if (freqstr == "") {
		errors::fail(errors::Exit::EFREQ, 0,
		             "frequency value missing");
	}

	auto value = strtod(str, nullptr);
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

types::ms clas::ival(char const * const str) {
	std::string interval{str};
	for (char & ch : interval) { ch = std::tolower(ch); }

	if (interval == "") {
		errors::fail(errors::Exit::EIVAL, 0, "interval value missing");
	}

	auto value = strtod(str, nullptr);
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

size_t clas::samples(char const * const str) {
	if (std::string{str} == "") {
		errors::fail(errors::Exit::ESAMPLES, 0,
		             "sample count value missing");
	}

	if (unit(str) != Unit::SCALAR) {
		errors::fail(errors::Exit::ESAMPLES, 0,
		             "sample count must be a scalar integer: "_s + str);
	}
	auto const cnt = strtol(str, nullptr, 0);
	auto const cntf = strtod(str, nullptr);
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

types::decikelvin_t clas::temperature(char const * const str) {
	std::string tempstr{str};
	for (char & ch : tempstr) { ch = std::toupper(ch); }

	if (tempstr == "") {
		errors::fail(errors::Exit::ETEMPERATURE, 0,
		             "temperature value missing");
	}

	auto value = strtod(str, nullptr);
	switch (unit(tempstr)) {
	case Unit::SCALAR:
	case Unit::CELSIUS:
		value += 273.15;
	case Unit::KELVIN:
		break;
	case Unit::FAHRENHEIT:
		value += 459.67;
	case Unit::RANKINE:
		value *= 5. / 9.;
		break;
	default:
		errors::fail(errors::Exit::ETEMPERATURE, 0,
		             "temperature value not recognised: "_s + str);
	}
	if (value < 0) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "temperature must be above absolute zero (-273.15 C): "_s + str);
	}
	return types::decikelvin_t(value * 10);
}

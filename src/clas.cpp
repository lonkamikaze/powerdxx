/**
 * Implements functions to process command line arguments.
 *
 * @file
 */

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
 */
struct Value {
	/**
	 * The magnitude of the value.
	 */
	double value;

	/**
	 * The unit of the value.
	 */
	Unit unit;

	/**
	 * Implicitly cast to the magnitude.
	 *
	 * @return
	 *	The magnitude of the value
	 */
	operator double() const { return this->value; }

	/**
	 * Implicitly cast to the unit.
	 *
	 * @return
	 *	The unit of the value
	 */
	operator Unit() const { return this->unit; }

	/**
	 * Add offset to the magnitude.
	 *
	 * @param off
	 *	The offset value
	 * @return
	 *	A self reference
	 */
	Value & operator +=(double const off) {
		return this->value += off, *this;
	}

	/**
	 * Subtract offset from the magnitude.
	 *
	 * @param off
	 *	The offset value
	 * @return
	 *	A self reference
	 */
	Value & operator -=(double const off) {
		return this->value -= off, *this;
	}

	/**
	 * Scale magnitude by the given factor.
	 *
	 * @param fact
	 *	The factor to scale the magnitude by
	 * @return
	 *	A self reference
	 */
	Value & operator *=(double const fact) {
		return this->value *= fact, *this;
	}

	/**
	 * Divide the magnitude by the given divisor.
	 *
	 * @param div
	 *	The divisor to divide the magnitude by
	 * @return
	 *	A self reference
	 */
	Value & operator /=(double const div) {
		return this->value /= div, *this;
	}

	/**
	 * Construct value from a null terminated character array.
	 *
	 * @param valp
	 *	A pointer to the value portion of the array
	 * @param unitp
	 *	Set by the constructor to point behind the magnitude
	 */
	Value(char const * const valp, char * unitp = nullptr) :
	    value{std::strtod(valp, &unitp)}, unit{Unit::UNKNOWN} {
		for (size_t unit = 0;
		     unitp && unit < utility::countof(UnitStr); ++unit) {
			auto unitstr = UnitStr[unit];
			for (size_t i = 0;
			     std::tolower(unitp[i]) == std::tolower(unitstr[i]);
			     ++i) {
				if (!unitstr[i]) {
					this->unit = static_cast<Unit>(unit);
					return;
				}
			}
		}
	}

};

} /* namespace */

types::cptime_t clas::load(char const * const str) {
	if (!str || !*str) {
		errors::fail(errors::Exit::ELOAD, 0,
		             "load target value missing");
	}

	auto value = Value{str};
	switch (value) {
	case Unit::SCALAR:
		if (value > 1. || value < 0) {
			errors::fail(errors::Exit::EOUTOFRANGE, 0,
			             "load targets must be in the range [0.0, 1.0]");
		}
		/* convert load to [0, 1024] range */
		value *= 1024;
		return value < 1 ? 1 : value;
	case Unit::PERCENT:
		if (value > 100. || value < 0) {
			errors::fail(errors::Exit::EOUTOFRANGE, 0,
			             "load targets must be in the range [0%, 100%]");
		}
		/* convert load to [0, 1024] range */
		value *= 10.24;
		return value < 1 ? 1 : value;
	default:
		break;
	}
	errors::fail(errors::Exit::ELOAD, 0, "load target not recognised");
}

types::mhz_t clas::freq(char const * const str) {
	if (!str || !*str) {
		errors::fail(errors::Exit::EFREQ, 0,
		             "frequency value missing");
	}

	auto value = Value{str};
	switch (value) {
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
		             "frequency value not recognised");
	}
	if (value > 1000000. || value < 0) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "target frequency must be in the range [0Hz, 1THz]");
	}
	return types::mhz_t(value);
}

types::ms clas::ival(char const * const str) {
	if (!str || !*str) {
		errors::fail(errors::Exit::EIVAL, 0, "interval value missing");
	}

	auto value = Value{str};
	if (value < 0) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "interval must be positive");
	}
	switch (value) {
	case Unit::SECOND:
		return types::ms{static_cast<long long>(value * 1000.)};
	case Unit::SCALAR: /* for powerd compatibility */
	case Unit::MILLISECOND:
		return types::ms{static_cast<long long>(value)};
	default:
		break;
	}
	errors::fail(errors::Exit::EIVAL, 0,
	             "interval not recognised");
}

size_t clas::samples(char const * const str) {
	if (!str || !*str) {
		errors::fail(errors::Exit::ESAMPLES, 0,
		             "sample count value missing");
	}

	auto value = Value{str};
	if (value != Unit::SCALAR) {
		errors::fail(errors::Exit::ESAMPLES, 0,
		             "sample count must be a scalar integer");
	}
	if (value != static_cast<size_t>(value)) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "sample count must be an integer");
	}
	if (value < 1 || value > 1000) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "sample count must be in the range [1, 1000]");
	}
	return value;
}

types::decikelvin_t clas::temperature(char const * const str) {
	if (!str || !*str) {
		errors::fail(errors::Exit::ETEMPERATURE, 0,
		             "temperature value missing");
	}

	auto value = Value{str};
	switch (value) {
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
		             "temperature value not recognised");
	}
	if (value < 0) {
		errors::fail(errors::Exit::EOUTOFRANGE, 0,
		             "temperature must be above absolute zero (-273.15 C): "s + str);
	}
	return value *= 10;
}

char const * clas::sysctlname(char const * const str) {
	using namespace utility::literals;
	using utility::highlight;
	using errors::fail;
	using errors::Exit;

	if (!str || !*str) {
		fail(Exit::ESYSCTLNAME, 0, "sysctl name missing");
	}

	for (auto it = str; it && *it; ++it) {
		/* pass permitted characters */
		if ((*it >= '0' && *it <= '9') ||
		    (*it >= 'A' && *it <= 'Z') ||
		    (*it >= 'a' && *it <= 'z') ||
		    *it == '.' ||
		    *it == '_' ||
		    *it == '-' ||
		    *it == '%') {
			continue;
		}
		auto const hl = highlight(str, it - str);
		/* utf-8 multy-byte fragments */
		if ((*it & 0xc0) == 0x80) {
			fail(Exit::ESYSCTLNAME, 0,
			     "multi-byte (utf-8) character fragment embedded in sysctl name:"s +
			     "\n\t" + hl.text + "\n\t" + hl.line);
		}
		/* utf-8 multi-byte heads */
		if ((*it & 0xe0) == 0xc0 ||  /* 2-byte */
		    (*it & 0xf0) == 0xe0 ||  /* 3-byte */
		    (*it & 0xf8) == 0xf0) {  /* 4-byte */
			fail(Exit::ESYSCTLNAME, 0,
			     "multi-byte (utf-8) character embedded in sysctl name:"s +
			     "\n\t" + hl.text + "\n\t" + hl.line);
		}
		/* non-ascii and non-utf-8 code points */
		if (*it < 0) {
			fail(Exit::ESYSCTLNAME, 0,
			     "invalid code point embedded in sysctl name:"s +
			     "\n\t" + hl.text + "\n\t" + hl.line);
		}
		/* ascii control characters */
		if (*it < ' ' || *it == 0x7f) {
			fail(Exit::ESYSCTLNAME, 0,
			     "control character embedded in sysctl name:"s +
			     "\n\t" + hl.text + "\n\t" + hl.line);
		}
		/* regular forbidden character */
		fail(Exit::ESYSCTLNAME, 0,
		     "forbidden character in sysctl name:"s +
		     "\n\t" + hl.text + "\n\t" + hl.line);
	}
	return str;
}

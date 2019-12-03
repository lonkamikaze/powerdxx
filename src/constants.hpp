/**
 * Defines a collection of constants.
 *
 * @file
 */

#include "types.hpp"

#ifndef _POWERDXX_CONSTANTS_HPP_
#define _POWERDXX_CONSTANTS_HPP_

/**
 * A collection of constants.
 */
namespace constants {

/*
 * Sysctl symbolic MIB representations.
 */

/**
 * The MIB name for per-CPU time statistics.
 */
char const * const CP_TIMES = "kern.cp_times";

/**
 * The MIB name for the AC line state.
 */
char const * const ACLINE = "hw.acpi.acline";

/**
 * The MIB name for CPU frequencies.
 */
char const * const FREQ = "dev.cpu.%d.freq";

/**
 * The MIB name for CPU frequency levels.
 */
char const * const FREQ_LEVELS = "dev.cpu.%d.freq_levels";

/**
 * The MIB name for CPU temperatures.
 */
char const * const TEMPERATURE = "dev.cpu.%d.temperature";

/**
 * An array of maximum temperature sources.
 */
char const * const TJMAX_SOURCES[] = {
	"dev.cpu.%d.coretemp.tjmax"
};

/*
 * Default values.
 */

/**
 * Default maximum clock frequency value.
 */
types::mhz_t const FREQ_DEFAULT_MAX{1000000};

/**
 * Default minimum clock frequency value.
 */
types::mhz_t const FREQ_DEFAULT_MIN{0};

/**
 * Clock frequency representing an uninitialised value.
 */
types::mhz_t const FREQ_UNSET{1000001};

/**
 * The default pidfile name of powerd.
 */
char const * const POWERD_PIDFILE = "/var/run/powerd.pid";

/**
 * The load target for adaptive mode, equals 50% load.
 */
types::cptime_t const ADP{512};

/**
 * The load target for hiadaptive mode, equals 37.5% load.
 */
types::cptime_t const HADP{384};

/**
 * The default temperautre offset between high and critical temperature.
 */
types::decikelvin_t const HITEMP_OFFSET{100};

} /* namespace constants */

#endif /* _POWERDXX_CONSTANTS_HPP_ */

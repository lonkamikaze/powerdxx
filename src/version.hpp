/**
 * Defines types and constants used for version management.
 *
 * @file
 */

#ifndef _POWERDXX_VERSION_HPP_
#define _POWERDXX_VERSION_HPP_

#include "utility.hpp"  /* utility::to_value() */

#include <cstdint>      /* uint64_t, uint8_t */

/**
 * Version information constants and types.
 */
namespace version {

/**
 * The pseudo MIB name for the load recording feature flags.
 */
char const * const LOADREC_FEATURES = "usr.app.powerdxx.loadrec.features";

/**
 * The data type to use for feature flags.
 */
typedef uint64_t flag_t;

/**
 * Feature flags for load recordings.
 */
enum class LoadrecBits {
	FREQ_TRACKING,  /**< Record clock frequencies per frame. */
};

/**
 * Literals to set flag bits.
 */
namespace literals {

/**
 * Set the FREQ_TRACKING bit.
 *
 * @param value
 *	The bit value
 * @return
 *	The flag at the correct bit position
 */
constexpr flag_t operator ""_FREQ_TRACKING(unsigned long long int value) {
	return static_cast<flag_t>(value > 0) <<
	       utility::to_value(LoadrecBits::FREQ_TRACKING);
}

} /* namespace literals */

} /* namespace version */

#endif /* _POWERDXX_VERSION_HPP_ */

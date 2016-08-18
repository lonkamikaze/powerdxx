/** \file
 * A collection of type aliases.
 */

#include <chrono>    /* std::chrono::milliseconds */

#ifndef _POWERDXX_TYPES_HPP_
#define _POWERDXX_TYPES_HPP_

/**
 * A collection of type aliases.
 */
namespace types {

/**
 * Millisecond type for polling intervals.
 */
typedef std::chrono::milliseconds ms;

/**
 * Type for CPU core indexing.
 */
typedef int coreid_t;

/**
 * Type for load counting.
 *
 * According to src/sys/kern/kern_clock.c the type is `long` (an array of
 * loads  `long[CPUSTATES]` is defined).
 * But in order to have defined wrapping characteristics `unsigned long`
 * will be used here.
 */
typedef unsigned long cptime_t;

/**
 * Type for CPU frequencies in MHz.
 */
typedef unsigned int mhz_t;

} /* namespace types */

#endif /* _POWERDXX_TYPES_HPP_ */

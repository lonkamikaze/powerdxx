/** \file
 * Implements generally useful functions.
 */

#include <cstdio>    /* snprintf() */

#ifndef _POWERDXX_UTILITY_HPP_
#define _POWERDXX_UTILITY_HPP_

/**
 * A collection of generally useful functions.
 */
namespace utility {

/**
 * Like sizeof(), but it returns the number of elements an array consists
 * of instead of the number of bytes.
 *
 * @tparam T,Count
 *	The type and number of array elements
 * @return
 *	The number of array entries
 */
template <typename T, size_t Count>
constexpr size_t countof(T (&)[Count]) { return Count; }

/**
 * Contains literals.
 */
namespace literals {

/**
 * A string literal operator equivalent to the `operator "" s` literal
 * provided by C++14 in \<string\>.
 *
 * @param op
 *	The raw string to turn into an std::string object
 * @param size
 *	The size of the raw string
 * @return
 *	An std::string instance
 */
inline std::string operator "" _s(char const * const op, size_t const size) {
	return {op, size};
}

} /* namespace literals */

/**
 * A wrapper around snprintf() that automatically pulls in the
 * destination buffer size.
 *
 * @tparam Size
 *	The destination buffer size
 * @tparam Args
 *	The types of the arguments
 * @param dst
 *	A reference to the destination buffer
 * @param format
 *	A printf style formatting string
 * @param args
 *	The printf arguments
 * @return
 *	The number of characters in the resulting string, regardless of the
 *	available space
 */
template <size_t Size, typename... Args>
inline int sprintf(char (& dst)[Size], const char * const format,
                   Args const... args) {
	return snprintf(dst, Size, format, args...);
}

} /* namespace utility */

#endif /* _POWERDXX_UTILITY_HPP_ */

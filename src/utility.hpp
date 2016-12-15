/** \file
 * Implements generally useful functions.
 */

#include <type_traits> /* std::underlying_type */
#include <string>

#include <cstdio>      /* snprintf() */

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

/**
 * Casts an enum to its underlying value.
 *
 * @tparam ET,VT
 *	The enum and value type
 * @param op
 *	The operand to convert
 * @return
 *	The integer representation of the operand
 */
template <class ET, typename VT = typename std::underlying_type<ET>::type>
constexpr VT to_value(ET const op) {
	return static_cast<VT>(op);
}

/**
 * A formatting wrapper around string literals.
 *
 * Overloads operator (), which treats the string as a printf formatting
 * string, the arguments represent the data to format.
 *
 * In combination with the literal _fmt, it can be used like this:
 *
 * ~~~ c++
 * std::cout << "%-15.15s %#018p\n"_fmt("Address:", this);
 * ~~~
 *
 * @tparam BufSize
 *	The buffer size for formatting, resulting strings cannot
 *	grow beyond `BufSize - 1`
 */
template <size_t BufSize>
class Formatter {
	private:
	/**
	 * Pointer to the string literal.
	 */
	char const * const fmt;

	public:
	/**
	 * Construct from string literal.
	 */
	constexpr Formatter(char const * const fmt) : fmt{fmt} {}

	/**
	 * Returns a formatted string.
	 *
	 * @tparam ArgTs
	 *	Variadic argument types
	 * @param args
	 *	Variadic arguments
	 * @return
	 *	An std::string formatted according to fmt
	 */
	template <typename... ArgTs>
	std::string operator ()(ArgTs const &... args) const {
		char buf[BufSize];
		auto count = sprintf(buf, this->fmt, args...);
		if (count >= BufSize) {
			/* does not fit into buffer */
			return {buf, BufSize - 1};
		} else if (count < 0) {
			/* encoding error */
			return {};
		}
		return {buf, static_cast<size_t>(count)};
	}
};

namespace literals {
/**
 * Literal to convert a string literal to a Formatter instance.
 *
 * @param fmt
 *	A printf style format string
 * @return
 *	A Formatter instance
 */
constexpr Formatter<16384> operator "" _fmt(char const * const fmt, size_t const) {
	return {fmt};
}
} /* namespace literals */

} /* namespace utility */

#endif /* _POWERDXX_UTILITY_HPP_ */

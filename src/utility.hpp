/**
 * Implements generally useful functions.
 *
 * @file
 */

#include <type_traits> /* std::underlying_type */
#include <charconv>    /* std::from_chars() */
#include <cctype>      /* std::isspace() */
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
 * This is a safeguard against accidentally using sprintf().
 *
 * Using it triggers a static_assert(), preventing compilation.
 *
 * @tparam Args
 *	Catch all arguments
 */
template <typename... Args>
inline void sprintf(Args...) {
	/* Assert depends on Args so it can only be determined if
	 * the function is actually instantiated. */
	static_assert(sizeof...(Args) && false,
	              "Use of sprintf() is unsafe, use sprintf_safe() instead");
}

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
inline int sprintf_safe(char (& dst)[Size], char const * const format,
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
		auto count = sprintf_safe(buf, this->fmt, args...);
		if (count < 0) {
			/* encoding error */
			return {};
		} else if (static_cast<size_t>(count) >= BufSize) {
			/* does not fit into buffer */
			return {buf, BufSize - 1};
		}
		return {buf, static_cast<size_t>(count)};
	}
};

/**
 * Contains literal operators.
 */
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

/**
 * A simple value container only allowing += and copy assignment.
 *
 * @tparam T
 *	The value type
 */
template <typename T>
class Sum {
	private:
	/**
	 * The sum of values accumulated.
	 */
	T value;

	public:
	/**
	 * Construct from an initial value.
	 *
	 * @param value
	 *	The initial value
	 */
	explicit constexpr Sum(T const & value) : value{value} {}

	/**
	 * Default construct.
	 */
	constexpr Sum() : Sum{0} {}

	/**
	 * Returns the current sum of values.
	 *
	 * @return
	 *	The sum of values by const reference
	 */
	constexpr operator T const &() const {
		return this->value;
	}

	/**
	 * Add a value to the sum.
	 *
	 * @param value
	 *	The value to add to the current sum
	 * @return
	 *	A self reference
	 */
	constexpr Sum & operator +=(T const & value) {
		this->value += value;
		return *this;
	}
};

/**
 * A simple value container that provides the minimum of assigned values.
 *
 * @tparam T
 *	The value type
 */
template <typename T>
class Min {
	private:
	/**
	 * The minimum of the assigned values.
	 */
	T value;

	public:
	/**
	 * Construct from an initial value.
	 *
	 * @param value
	 *	The initial value
	 */
	explicit constexpr Min(T const & value) : value{value} {}

	/**
	 * Returns the current minimum.
	 *
	 * @return
	 *	The minimum by const reference
	 */
	constexpr operator T const &() const {
		return this->value;
	}

	/**
	 * Assign a new value, if it is less than the current value.
	 *
	 * @param value
	 *	The value to assign
	 * @return
	 *	A self reference
	 */
	constexpr Min & operator =(T const & value) {
		this->value = this->value <= value ? this->value : value;
		return *this;
	}
};

/**
 * A simple value container that provides the maximum of assigned values.
 *
 * @tparam T
 *	The value type
 */
template <typename T>
class Max {
	private:
	/**
	 * The maximum of the assigned values.
	 */
	T value;

	public:
	/**
	 * Construct from an initial value.
	 *
	 * @param value
	 *	The initial value
	 */
	explicit constexpr Max(T const & value) : value{value} {}

	/**
	 * Returns the current maximum.
	 *
	 * @return
	 *	The maximum by const reference
	 */
	constexpr operator T const &() const {
		return this->value;
	}

	/**
	 * Assign a new value, if it is greater than the current value.
	 *
	 * @param value
	 *	The value to assign
	 * @return
	 *	A self reference
	 */
	constexpr Max & operator =(T const & value) {
		this->value = this->value >= value ? this->value : value;
		return *this;
	}
};

/**
 * A functor for reading numerical values from a string or character
 * array.
 */
struct FromChars {
	/**
	 * The next character to read.
	 */
	char const * it;

	/**
	 * The first character of the same array that may not be read,
	 * this should usually point to a terminating zero.
	 */
	char const * const end;

	/**
	 * Retrieve an integral or floating point value from the array.
	 *
	 * The operation may fail for multiple reasons:
	 *
	 * - No more characters left to read, in that case the functor
	 *   will equal false
	 * - The given characters do not represent a valid value, in
	 *   that case the functor will equal true
	 *
	 * @tparam T
	 *	The value type to retrieve
	 * @param dst
	 *	The lvalue to assign to
	 * @retval true
	 *	The numerical value was successfully read from the array
	 * @retval false
	 *	The numerical value could not be read from the array
	 */
	template <typename T>
	[[nodiscard]] bool operator ()(T & dst) {
		if (!this->it) {
			return false;
		}
		auto [p, ec] = std::from_chars(this->it, this->end, dst);
		if (this->it == p) {
			return false;
		}
		for (; p != this->end && std::isspace(*p); ++p);
		this->it = p;
		return true;
	}

	/**
	 * Check if unread characters remain.
	 *
	 * @retval false
	 *	All characters have been read
	 * @retval true
	 *	Characters remain to be read
	 */
	operator bool() const {
		return this->end != this->it;
	}

	/**
	 * Range base constructor.
	 *
	 * @param start,end
	 *	The character array range
	 */
	FromChars(char const * const start, char const * const end) :
	    it{start}, end{end} {
		for (; this->it != end && std::isspace(*this->it); ++this->it);
	}

	/**
	 * Construct from a character array.
	 *
	 * @tparam CountV
	 *	The number of characters
	 * @param str
	 *	Tha character array to parse from
	 * @param terminator
	 *	Indicates whether the character array has a terminating
	 *	null character.
	 */
	template <size_t CountV>
	FromChars(char const (& str)[CountV], bool terminator = true) :
	    FromChars{str, str + CountV - terminator} {}

	/**
	 * Construct functor from a string.
	 *
	 * Note that changing the string during the lifetime of the
	 * functor may silently invalidate the functor's state and
	 * thus invoke undefined behaviour.
	 *
	 * @param str
	 *	The string to parse from
	 */
	FromChars(std::string const & str) :
	    FromChars{str.data(), str.data() + str.size()} {}
};

} /* namespace utility */

#endif /* _POWERDXX_UTILITY_HPP_ */

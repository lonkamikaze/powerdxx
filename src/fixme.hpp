/** \file
 * Implementations in the fixme namespace.
 */

#include <sstream>  /**< std::ostringstream */

#ifndef _POWERDXX_FIXME_HPP_
#define _POWERDXX_FIXME_HPP_
/**
 * Workarounds for compiler/library bugs.
 */
namespace fixme {

/**
 * G++ 5.3 does not believe in std::to_string().
 *
 * @tparam T
 *	The argument type to convert
 * @param op
 *	The argument to convert
 * @return
 *	A string of the given argument
 */
template <typename T>
inline std::string to_string(T const & op) {
	std::ostringstream result;
	result << op;
	return result.str();
}

} /* namespace fixme */

#endif /* _POWERDXX_FIXME_HPP_ */

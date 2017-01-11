/** \file
 * Provides system call error handling.
 */

#ifndef _POWERDXX_SYS_ERROR_HPP_
#define _POWERDXX_SYS_ERROR_HPP_

#include <cerrno>          /* errno */
#include <cstring>         /* strerror() */

/**
 * Wrappers around native system interfaces.
 */
namespace sys {

/**
 * Can be thrown by syscall function wrappers if the function returned
 * with an error.
 *
 * This is its own type for easy catching, but implicitly casts to
 * int for easy comparison.
 *
 * @tparam Domain
 *	A type marking the domain the error comes from, e.g. sys::ctl::error
 */
template <class Domain>
struct sc_error {
	/**
	 * The errno set by the native C function.
	 */
	int error;

	/**
	 * Cast to integer.
	 *
	 * @return
	 *	The errno code
	 */
	operator int() const {
		return this->error;
	}

	/**
	 * Return c style string.
	 *
	 * @return
	 *	A string representation of the error
	 */
	char const * c_str() const {
		return ::strerror(this->error);
	}
};

} /* namespace sys */

#endif /* _POWERDXX_SYS_ERROR_HPP_ */

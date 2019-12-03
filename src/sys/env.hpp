/**
 * Implements zero-cost abstractions for the getenv(3) facilities.
 *
 * @file
 */

#ifndef _POWERDXX_SYS_ENV_HPP_
#define _POWERDXX_SYS_ENV_HPP_

#include "error.hpp"    /* sys::sc_error */

#include <cstdlib>      /* getenv(), setenv() etc. */

namespace sys {

/**
 * Provides wrappers around the getenv() family of functions.
 */
namespace env {

/**
 * The domain error type.
 */
struct error {};

/**
 * A reference type refering to an environment variable.
 *
 * To avoid issues with the lifetime of the name string this is not
 * copy constructible or assignable.
 */
class Var {
	private:
	/**
	 * A pointer to the variable name.
	 */
	char const * const name;

	public:
	/**
	 * Construct an environment variable reference.
	 *
	 * @tparam Size
	 *	The size of the name buffer
	 * @param name
	 *	The name of the environment variable
	 */
	template <size_t Size>
	Var(char const (& name)[Size]) : name{name} {}

	/**
	 * Do not permit copy construction.
	 */
	Var(Var const &) = delete;

	/**
	 * Do not permit copy assignment.
	 */
	Var & operator =(Var const &) = delete;

	/**
	 * Retrieve the value of the environment variable.
	 *
	 * @return
	 *	A pointer to the character array with the variable value
	 * @retval nullptr
	 *	The variable does not exist
	 */
	operator char const *() const {
		return getenv(this->name);
	}

	/**
	 * Assign a new value to the environment variable.
	 *
	 * Deletes the variable if nullptr is assigned.
	 *
	 * @param assign
	 *	The new value
	 * @return
	 *	A self-reference
	 * @throws sc_error<error>{EINVAL}
	 *	Invalid variable name
	 * @throws sc_error<error>{ENOMEM}
	 *	Failed to allocate memory when updating the environment
	 */
	Var & operator =(char const * const assign) {
		auto const result =
		    assign
		    ? setenv(this->name, assign, 1)
		    : unsetenv(this->name);
		if (0 != result) {
			throw sc_error<error>{errno};
		}
		return *this;
	}

	/**
	 * Explicitly deletes the environment variable.
	 *
	 * @return
	 *	A self-reference
	 * @throws sc_error<error>{EINVAL}
	 *	Invalid variable name
	 * @throws sc_error<error>{ENOMEM}
	 *	Failed to allocate memory when updating the environment
	 */
	Var & erase() {
		return *this = nullptr;
	}

	/**
	 * Explicitly retrieve the value as a character array.
	 *
	 * @return
	 *	A pointer to the character array with the variable value
	 * @retval nullptr
	 *	The variable does not exist
	 */
	char const * c_str() const {
		return *this;
	}
};

/**
 * A singleton class providing access to environment variables.
 */
struct Vars {
	/**
	 * Access environment variable by name.
	 *
	 * @tparam T
	 *	The name argument type
	 * @param name
	 *	The name of the variable by reference
	 */
	template <typename T>
	Var const operator [](T const & name) const {
		return {name};
	}

	/**
	 * Access environment variable by name.
	 *
	 * @tparam T
	 *	The name argument type
	 * @param name
	 *	The name of the variable by reference
	 */
	template <typename T>
	Var operator [](T const & name) {
		return {name};
	}
} vars; /**< Singleton providing access to environment variables. */

} /* namespace env */

} /* namespace sys */

#endif /* _POWERDXX_SYS_ENV_HPP_ */

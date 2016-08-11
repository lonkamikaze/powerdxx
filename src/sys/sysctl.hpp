/** \file
 * Implements safer c++ wrappers for the sysctl() interface.
 */

#ifndef _POWERDXX_SYS_SYSCTL_HPP_
#define _POWERDXX_SYS_SYSCTL_HPP_

#include "error.hpp"       /* sys::sc_error */

#include <sys/types.h>     /* sysctl() */
#include <sys/sysctl.h>    /* sysctl() */

namespace sys {

/**
 * This namespace contains safer c++ wrappers for the sysctl() interface.
 *
 * The template class Sysctl represents a sysctl address and offers
 * handles to retrieve or set the stored value.
 */
namespace ctl {

/**
 * Management Information Base identifier type (see sysctl(3)).
 */
typedef int mib_t;

/**
 * Represents a sysctl MIB address.
 *
 * @tparam MibDepth
 *	The maximum allowed MIB depth
 */
template <size_t MibDepth>
class Sysctl {
	private:
	/**
	 * Stores the MIB address.
	 */
	mib_t mib[MibDepth];

	public:
	/**
	 * The default constructor.
	 *
	 * This is available to defer initialisation to a later moment.
	 * This might be useful when initialising global or static
	 * instances by a character string repesented name.
	 */
	constexpr Sysctl() : mib{} {}

	/**
	 * Initialise the MIB address from a character string.
	 *
	 * @param name
	 *	The name of the sysctl
	 * @throws sc_error
	 *	May throw an exception if the addressed sysct does
	 *	not exist or if the address is too long to store
	 */
	Sysctl(char const * const name) {
		size_t length = MibDepth;
		if (::sysctlnametomib(name, this->mib, &length) == -1) {
			throw sc_error{errno};
		}
	}

	/**
	 * Initialise the MIB address directly.
	 *
	 * Some important sysctl values have a fixed address that
	 * can be initialised at compile time with a noexcept guarantee.
	 *
	 * Spliting the MIB address into head and tail makes sure
	 * that `Sysctl(char *)` does not match the template and is
	 * instead implicitly cast to invoke `Sysctl(char const *)`.
	 *
	 * @tparam Tail
	 *	The types of the trailing MIB address values (must
	 *	be mib_t)
	 * @param head,tail
	 *	The mib
	 */
	template <typename... Tail>
	constexpr Sysctl(mib_t const head, Tail const... tail) noexcept :
	    mib{head, tail...} {
		static_assert(MibDepth >= sizeof...(Tail) + 1,
		              "The number of MIB addresses must not exceed the MIB depth");
	}

	/**
	 * Update the given buffer with a value retrieved from the
	 * sysctl.
	 *
	 * @param buf,bufsize
	 *	The target buffer and its size
	 * @throws sc_error
	 *	Throws if value retrieval fails or is incomplete,
	 *	e.g. because the value does not fit into the target
	 *	buffer
	 */
	void update(void * const buf, size_t const bufsize) const {
		auto len = bufsize;
		if (::sysctl(this->mib, MibDepth, buf, &len, nullptr, 0)
		    == -1) {
			throw sc_error{errno};
		}
	}

	/**
	 * Update the given value with a value retreived from the
	 * sysctl.
	 *
	 * @tparam T
	 *	The type store the sysctl value in
	 * @param value
	 *	A reference to the target value
	 * @throws sc_error
	 *	Throws if value retrieval fails or is incomplete,
	 *	e.g. because the value does not fit into the target
	 *	type
	 */
	template <typename T>
	void update(T & value) const {
		update(&value, sizeof(T));
	}

	/**
	 * Retrieve an array from the sysctl address.
	 *
	 * This is useful to retrieve variable length sysctls, like
	 * characer strings.
	 *
	 * @tparam T
	 *	The type stored in the array
	 * @return
	 *	And array of T with the right length to store the
	 *	whole sysctl value
	 * @throws sc_error
	 *	May throw if the size of the sysctl increases after
	 *	the length was queried
	 */
	template <typename T>
	std::unique_ptr<T[]> get() const {
		size_t len = 0;
		if (::sysctl(this->mib, MibDepth, nullptr, &len, nullptr, 0)
		    == -1) {
			throw sc_error{errno};
		}
		auto result = std::unique_ptr<T[]>(new T[len / sizeof(T)]);
		update(result.get(), len);
		return result;
	}

	/**
	 * Update the the sysctl value with the given buffer.
	 *
	 * @param buf,bufsize
	 *	The source buffer
	 * @throws sc_error
	 *	If the source buffer cannot be stored in the sysctl
	 */
	void set(void const * const buf, size_t const bufsize) {
		if (::sysctl(this->mib, MibDepth, nullptr, nullptr,
		             buf, bufsize) == -1) {
			throw sc_error{errno};
		}
	}

	/**
	 * Update the the sysctl value with the given value.
	 *
	 * @tparam T
	 *	The value type
	 * @param value
	 *	The value to set the sysctl to
	 */
	template <typename T>
	void set(T const & value) {
		set(&value, sizeof(T));
	}

};

template <typename... MibTs>
constexpr Sysctl<sizeof...(MibTs)> make_Sysctl(MibTs const... mib) {
	return {mib...};
}

template <size_t MibDepth, typename T>
class SysctlValue {
	private:
	T value;
	Sysctl<MibDepth> sysctl;

	public:
	template <typename... SysctlArgs>
	constexpr SysctlValue(T const & value, SysctlArgs const &... args) :
		value{value}, sysctl{args...} {};

	SysctlValue & operator =(T const & value) {
		this->value = value;
		this->sysctl.set(this->value);
		return *this;
	}

	operator T const &() const {
		return this->value;
	}

	void update() {
		this->sysctl.update(this->value);
	}
};

template <typename T, typename... MibTs>
constexpr SysctlValue<sizeof...(MibTs), T>
make_SysctlValue(T const & value, MibTs const... addr) {
	return {value, addr...};
}

} /* namespace ctl */
} /* namespace sys */

#endif /* _POWERDXX_SYS_SYSCTL_HPP_ */

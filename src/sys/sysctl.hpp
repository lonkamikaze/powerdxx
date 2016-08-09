/** \file
 * Implements safer c++ wrappers for the sysctl() interface.
 */

#ifndef _POWERDXX_SYS_SYSCTL_HPP_
#define _POWERDXX_SYS_SYSCTL_HPP_

#include "error.hpp"

#include <sys/types.h>     /* sysctl() */
#include <sys/sysctl.h>    /* sysctl() */

namespace sys {

/**
 * Management Information Base identifier type (see sysctl(3)).
 */
typedef int mib_t;

template <size_t MibDepth>
class Sysctl {
	private:
	mib_t addr[MibDepth];

	public:
	constexpr Sysctl() : addr{} {}

	Sysctl(char const * const name) {
		size_t length = MibDepth;
		if (::sysctlnametomib(name, this->addr, &length) == -1) {
			throw sc_error{errno};
		}
		assert(MibDepth >= length &&
		       "The MIB depth should match the returned length");
	}

	Sysctl(char * const name) :
	Sysctl{const_cast<char const * const>(name)} {}

	template <typename... MibTs>
	constexpr Sysctl(MibTs const... addr) : addr{addr...} {
		static_assert(MibDepth >= sizeof...(MibTs),
		              "The number of MIB addresses must not exceed the MIB depth");
	}

	void update(void * const valuep, size_t const valuelen) const {
		auto len = valuelen;
		if (::sysctl(this->addr, MibDepth, valuep, &len, nullptr, 0)
		    == -1) {
			throw sc_error{errno};
		}
		assert(len == valuelen &&
		       "buffer size must match the data returned");
	}

	template <typename T>
	void update(T & value) const {
		update(&value, sizeof(T));
	}

	template <typename T>
	std::unique_ptr<T[]> get() const {
		size_t len;
		if (::sysctl(this->addr, MibDepth, nullptr, &len, nullptr, 0)
		    == -1) {
			throw sc_error{errno};
		}
		auto result = std::unique_ptr<T[]>(new T[len / sizeof(T)]);
		update(result.get(), len);
		return result;
	}

	void set(void const * valuep, size_t const valuelen) {
		if (::sysctl(this->addr, MibDepth, nullptr, nullptr,
		             valuep, valuelen) == -1) {
			throw sc_error{errno};
		}
	}

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

} /* namespace sys */

#endif /* _POWERDXX_SYS_SYSCTL_HPP_ */

/**
 * Implements a c++ wrapper for the signal(3) call.
 *
 * @file
 */

#ifndef _POWERDXX_SYS_SIGNAL_HPP_
#define _POWERDXX_SYS_SIGNAL_HPP_

#include "error.hpp"    /* sys::sc_error */

#include <csignal>

namespace sys {

/**
 * This namespace provides c++ wrappers for signal(3).
 */
namespace sig {

/**
 * The domain error type.
 */
struct error {};

/**
 * Convenience type for signal handlers.
 */
using sig_t = void (*)(int);

/**
 * Sets up a given signal handler and restores the old handler when
 * going out of scope.
 */
class Signal {
	private:
	/**
	 * The signal this handler is handling.
	 */
	int const sig;

	/**
	 * The previous signal handler.
	 */
	sig_t const handler;

	public:
	/**
	 * Sets up the given handler.
	 *
	 * @param sig
	 *	The signal to set a handler for
	 * @param handler
	 *	The signal handling function
	 * @throws sys::sc_error<error>
	 *	Throws with the errno of signal()
	 */
	Signal(int const sig, sig_t const handler) :
	    sig{sig}, handler{::signal(sig, handler)} {
		if (this->handler == SIG_ERR) {
			throw sc_error<error>{errno};
		}
	}

	/**
	 * Restore previous signal handler.
	 */
	~Signal() {
		::signal(this->sig, this->handler);
	}
};

} /* namespace sig */

} /* namespace sys */

#endif /* _POWERDXX_SYS_SIGNAL_HPP_ */

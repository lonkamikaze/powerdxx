/** \file
 * Implements safer c++ wrappers for the pidfile_*() interface.
 */

#ifndef _POWERDXX_SYS_PIDFILE_HPP_
#define _POWERDXX_SYS_PIDFILE_HPP_

#include "error.hpp"    /* sys::sc_error */

#include <libutil.h>    /* pidfile_*() */

namespace sys {

/**
 * This namespace contains safer c++ wrappers for the pidfile_*() interface.
 *
 * The class Pidfile implements the RAII pattern for holding a pidfile.
 */
namespace pid {

/**
 * The domain error type.
 */
struct error {};

/**
 * A wrapper around the pidfile_* family of commands implementing the
 * RAII pattern.
 */
class Pidfile final {
	private:
	/**
	 * In case of failure to acquire the lock, the PID of the other
	 * process holding it is stored here.
	 */
	pid_t otherpid;

	/**
	 * Pointer to the pidfile state data structure.
	 *
	 * Thus is allocated by pidfile_open() and assumedly freed by
	 * pidfile_remove().
	 */
	pidfh * pfh;

	public:
	/**
	 * Attempts to open the pidfile.
	 *
	 * @param pfname,mode
	 *	Arguments to pidfile_open()
	 * @throws pid_t
	 *	Throws the PID of the other process already holding *	the requested pidfile
	 * @throws sys::sc_error<error>
	 *	Throws with the errno of pidfile_open()
	 */
	Pidfile(char const * const pfname, mode_t const mode) :
	    otherpid{0}, pfh{pidfile_open(pfname, mode, &this->otherpid)} {
		if (this->pfh == nullptr) {
			switch (errno) {
			case EEXIST:
				throw this->otherpid;
			default:
				throw sc_error<error>{errno};
			}
		}
	}

	/**
	 * Removes the pidfile.
	 */
	~Pidfile() {
		pidfile_remove(this->pfh);
	}

	/**
	 * Returns the PID of the other process holding the lock.
	 */
	pid_t other() { return this->otherpid; }

	/**
	 * Write PID to the file, should be called after daemon().
	 *
	 * @throws sys::sc_error<error>
	 *	Throws with the errno of pidfile_write()
	 */
	void write() {
		if (pidfile_write(this->pfh) == -1) {
			throw sc_error<error>{errno};
		}
	}
};

} /* namespace pid */

} /* namespace sys */

#endif /* _POWERDXX_SYS_PIDFILE_HPP_ */

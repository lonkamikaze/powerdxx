/** \file
 * Implements timing::Cycle, a cyclic sleep functor.
 */

#ifndef _POWERDXX_TIMING_CYCLE_HPP_
#define _POWERDXX_TIMING_CYCLE_HPP_

#include <chrono>    /* std::chrono::steady_clock::now() */
#include <unistd.h>  /* usleep() */

/**
 * Namespace for time management related functionality.
 */
namespace timing {

/**
 * Implements an interruptible cyclic sleeping functor.
 *
 * Cyclic sleeping means that instead of having a fixed sleeping
 * time, each sleep is timed to meet a fixed wakeup time. I.e. the
 * waking rhythm does not drift with changing system loads.
 *
 * The canonical way to do this in C++ is like this:
 *
 * ~~~ c++
 * #include <chrono>
 * #include <thread>
 *
 * int main() {
 * 	std::chrono::milliseconds const ival{500};
 * 	auto time = std::chrono::steady_clock::now();
 * 	while (…something…) {
 * 		std::this_thread::sleep_until(time += ival);
 * 		…do stuff…
 * 	}
 * 	return 0;
 * }
 * ~~~
 *
 * The issue is that you might want to install a signal handler to
 * guarantee stack unwinding and sleep_until() will resume its wait
 * after the signal handler completes.
 *
 * The Cycle class offers you an interruptible sleep:
 *
 * ~~~ c++
 * #include "Cycle.hpp"
 * #include <csignal>
 * …signal handlers…
 *
 * int main() {
 * 	std::chrono::milliseconds const ival{500};
 * 	…setup some signal handlers…
 * 	timing::Cycle sleep;
 * 	while (…something… && sleep(ival)) {
 * 		…do stuff…
 * 	}
 * 	return 0;
 * }
 * ~~~
 *
 * In the example the while loop is terminated if the `sleep()` is
 * interrupted by a signal. Optionally the sleep cycle can be resumed:
 *
 * ~~~
 * timing::Cycle sleep;
 * while (…something…) {
 *	if (!sleep(ival)) {
 *		…interrupted…
 * 		while (!sleep());
 *	}
 * 	…do stuff…
 * }
 * ~~~
 *
 * Note there was a design decision between providing a cycle time
 * to the constructor or providing it every cycle. The latter was
 * chosen so the cycle time can be adjusted.
 */
class Cycle {
	private:
	/**
	 * Use steady_clock, avoid time jumps.
	 */
	using clock = std::chrono::steady_clock;

	/**
	 * Shorthand for microseconds.
	 */
	using us = std::chrono::microseconds;

	/**
	 * The current time clock
	 */
	std::chrono::time_point<clock> clk = clock::now();

	public:
	/**
	 * Completes an interrupted sleep cycle.
	 *
	 * I.e. if the last sleep cycle was 500 ms and the sleep
	 * was interrupted 300 ms into the cycle, this would sleep
	 * for the remaining 200 ms unless interrupted.
	 *
	 * @retval true
	 *	Sleep completed uninterrupted
	 * @retval false
	 *	Sleep was interrupted
	 */
	bool operator ()() const {
		auto const remainingTime{
			std::chrono::duration_cast<us>(this->clk - clock::now())
		};
		auto const sleepDuration = remainingTime.count();
		return sleepDuration <= 0 || 0 == usleep(sleepDuration);
	}

	/**
	 * Sleep for the time required to complete the given cycle
	 * time.
	 *
	 * I.e. if the time since the last sleep cycle was 12 ms and
	 * the given cycleTime was 500 ms, the actual sleeping time
	 * would be 488 ms.
	 *
	 * @tparam DurTraits
	 *	The traits of the duration type
	 * @param cycleTime
	 *	The duration of the cycle to complete
	 * @retval true
	 *	Command completed uninterrupted
	 * @retval false
	 *	Command was interrupted
	 */
	template <class... DurTraits>
	bool operator ()(std::chrono::duration<DurTraits...> const & cycleTime) {
		this->clk += cycleTime;
		return (*this)();
	}

};

} /* namespace timing */

#endif /* _POWERDXX_TIMING_CYCLE_HPP_ */

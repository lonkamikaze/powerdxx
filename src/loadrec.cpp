
#include "types.hpp"
#include "constants.hpp"

#include "sys/sysctl.hpp"

#include <iostream>  /* std::cout, std::cerr */
#include <chrono>    /* std::chrono::steady_clock::now() */
#include <thread>    /* std::this_thread::sleep_until() */

#include <sys/resource.h>  /* CPUSTATES */

namespace {

using namespace types;
using namespace constants;

} /* namespace */

int main(int argc, char * argv[]) {
	sys::ctl::SysctlOnce<coreid_t, 2> const ncpu{0, {CTL_HW, HW_NCPU}};
	sys::ctl::Sysctl<2> const cp_times_ctl = {CP_TIMES};

	auto cp_times = std::unique_ptr<cptime_t[][CPUSTATES]>(
	    new cptime_t[2 * ncpu][CPUSTATES]{});

	auto time = std::chrono::steady_clock::now();
	//auto const start = time;
	size_t sample = 0;
	while (true) {
		std::this_thread::sleep_until(time += ms{50});
		cp_times_ctl.get(cp_times[sample * ncpu],
		                 ncpu * sizeof(cp_times[0]));
		for (size_t i = 0; i < ncpu; ++i) {
			std::cout << "cpu" << i << ':';
			for (size_t q = 0; q < CPUSTATES; ++q) {
				std::cout << ' '
				          << (cp_times[sample * ncpu + i][q] -
				              cp_times[((sample + 1) % 2) * ncpu + i][q]);
			}
			std::cout << '\n';
		}
		sample = (sample + 1) % 2;
	}
}


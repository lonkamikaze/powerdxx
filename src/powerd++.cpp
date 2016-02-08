
#include <iostream>
#include <string>
#include <cstdio>
#include <memory>
#include <cassert>
#include <cstdint> /* uint64_t */
#include <chrono>  /* std::chrono::milliseconds */
#include <thread>   /* std::this_thread::sleep_for() */

#include <sys/types.h>     /* sysctl() */
#include <sys/sysctl.h>    /* sysctl() */
#include <sys/resource.h>  /* CPUSTATES */

namespace {

using ms = std::chrono::milliseconds;
typedef int mib_t;
typedef int coreid_t;

/* use unsigned for defined overflow semantics */
typedef unsigned long cptime_t;

mib_t const NCPU_MIB[]{CTL_HW, HW_NCPU};

char const CP_TIMES[] = "kern.cp_times"; /* per-CPU time statistics */

char const ACLINE[] = "hw.acpi.acline"; /* AC line state */
enum class AcLineState {
	BATTERY = 0, ONLINE = 1, UNKNOWN = 2
};

char const FREQ[] =        "dev.cpu.%d.freq";

enum class Exit : int { OK, ESYSCTL, ENOFREQ, EFORBIDDEN };

const char * const ExitStr[]{"OK", "ESYSCTL", "ENOFREQ", "EFORBIDDEN"};

struct Core {
	mib_t freq_mib[4] = {0};
	coreid_t controller = -1;
	cptime_t idle = 0;
};

/* wrap all the mutable globals in a struct for clearer semantics */
struct {
	size_t samples_max = 8;
	size_t sample = 0;
	cptime_t target = 512;
	coreid_t ncpu;
	ms interval{500};

	mib_t cp_times_mib[2];
	/* see src/sys/kern/kern_clock.c for long[CPUSTATES] */
	std::unique_ptr<cptime_t[][CPUSTATES]> cp_times;

	std::unique_ptr<Core[]> cores;
} g;

void warn(std::string const & msg) {
	std::cerr << "powerd++: " << msg << '\n';
}

void fail(Exit const exitcode, std::string const & msg) {
	warn(std::string{'('} + ExitStr[static_cast<int>(exitcode)] + ") " + msg);
	throw exitcode;
}

void sysctl_fail(int err) {
	switch(err) {
	case 0:
		break;
	case EFAULT:
		fail(Exit::ESYSCTL, "sysctl failed with EFAULT");
		break;
	case EINVAL:
		fail(Exit::ESYSCTL, "sysctl failed with EINVAL");
		break;
	case ENOMEM:
		fail(Exit::ESYSCTL, "sysctl failed with ENOMEM");
		break;
	case ENOTDIR:
		fail(Exit::ESYSCTL, "sysctl failed with ENOTDIR");
		break;
	case EISDIR:
		fail(Exit::ESYSCTL, "sysctl failed with EISDIR");
		break;
	case ENOENT:
		fail(Exit::ESYSCTL, "sysctl failed with ENOENT");
		break;
	case EPERM:
		fail(Exit::ESYSCTL, "sysctl failed with EPERM");
		break;
	default:
		fail(Exit::ESYSCTL, "sysctl failed with unknown error");
	};
}

template <u_int Namelen>
void sysctl_get(int const (&name)[Namelen], void * oldp, size_t const oldplen) {
	auto len = oldplen;
	int err = ::sysctl(name, Namelen, oldp, &len, nullptr, 0);
	sysctl_fail(err & errno);
	assert(len == oldplen);
	assert(err == 0);
}

template <u_int Namelen, typename T, size_t OldpLen>
inline void sysctl_get(int const (&name)[Namelen], T (&oldp)[OldpLen]) {
	return sysctl_get(name, oldp, OldpLen * sizeof(T));
}

template <u_int Namelen, typename T>
inline void sysctl_get(int const (&name)[Namelen], T & oldv) {
	return sysctl_get(name, &oldv, sizeof(oldv));
}

template <u_int Namelen>
void sysctl_set(int const (&name)[Namelen], void const * newp, size_t const newplen) {
	int err = ::sysctl(name, Namelen, nullptr, nullptr, newp, newplen);
	sysctl_fail(err & errno);
	assert(err == 0);
}

template <u_int Namelen, typename T, size_t NewpLen>
inline void sysctl_set(int const (&name)[Namelen], T const (&newp)[NewpLen]) {
	return sysctl_set(name, newp, NewpLen * sizeof(T));
}

template <u_int Namelen, typename T>
inline void sysctl_set(int const (&name)[Namelen], T const & newv) {
	return sysctl_set(name, &newv, sizeof(newv));
}

/*
 * Wrapper around sysctlnametomib() that fails hard.
 *
 * Fails if:
 * - The length of the MIB structure does not match the returned length
 * - sysctlnametomib() returns a non-0 value
 */
template <size_t MibLen>
void sysctlnametomib(const char * const name, mib_t (&mibp)[MibLen]) {
	size_t length = MibLen;
	auto err = ::sysctlnametomib(name, mibp, &length);
	sysctl_fail(err & errno);
	assert(MibLen == length &&
	       "The MIB array length should match the returned MIB length");
}

/*
 * Wrapper around snprintf() that automatically takes the size of the given
 * destination array.
 */
template <size_t Size, typename... Args>
inline int sprintf(char (&dst)[Size], const char * format,
                   Args const... args) {
	return std::snprintf(dst, Size, format, args...);
}

void init() {
	/* number of cores */
	sysctl_get(NCPU_MIB, g.ncpu);

	/*
	 * Get the frequency controlling core for each core.
	 * Basically acts as if the kernel supported local frequency changes.
	 */
	g.cores = std::unique_ptr<Core[]>(new Core[g.ncpu]);
	char name[40];
	coreid_t controller = -1;
	for (coreid_t core = 0; core < g.ncpu; ++core) {
		sprintf(name, FREQ, core);
		try {
			sysctlnametomib(name, g.cores[core].freq_mib);
			controller = core;
		} catch (Exit &) {
			warn(name);
			if (0 > controller) {
				throw;
			}
		}
		g.cores[core].controller = controller;
	}

	/* MIB for kern.cp_times */
	sysctlnametomib(CP_TIMES, g.cp_times_mib);
	/* create buffer for system load times */
	g.cp_times = std::unique_ptr<cptime_t[][CPUSTATES]>(
	    new cptime_t[g.samples_max * g.ncpu][CPUSTATES]{{0}});
}

/*
 * Updates the cp_times ring buffer and the idle times for each core.
 */
void update_cp_times() {
	sysctl_get(g.cp_times_mib, g.cp_times[g.sample * g.ncpu],
	           g.ncpu * sizeof(g.cp_times[0]));

	for (coreid_t core = 0; core < g.ncpu; ++core) {
		auto const & cp_times = g.cp_times[g.sample * g.ncpu + core];
		auto const & cp_times_old = g.cp_times[((g.sample + 1) % g.samples_max) * g.ncpu + core];
		uint64_t all = 0;
		for (auto const time : cp_times) { all += time; }
		for (auto const time : cp_times_old) { all -= time; }

		cptime_t idle = cp_times[CP_IDLE] - cp_times_old[CP_IDLE];
		g.cores[core].idle = all ? cptime_t{(uint64_t{idle} << 10) / all} : 1024;
	}
	g.sample = (g.sample + 1) % g.samples_max;
}

void update_idle_times() {
	update_cp_times();

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto const & core = g.cores[corei];
		if (core.controller == corei) { continue; }
		auto & controller = g.cores[core.controller];
		if (controller.idle > core.idle) {
			controller.idle = core.idle;
		}
	}
}

void update_freq() {
	update_idle_times();

	assert(g.target > 0 && g.target <= 1024 &&
	       "idle target must be in the range (0, 1024]");

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto const & core = g.cores[corei];
		if (core.controller != corei) { continue; }

		int oldfreq;
		sysctl_get(core.freq_mib, oldfreq);
		assert(oldfreq == (((oldfreq << 10) & ~0x3ff) >> 10) &&
		       "CPU frequency exceeds values that are safe to compute");
		int freq = oldfreq * g.target / (core.idle ? core.idle : 1);
		sysctl_set(core.freq_mib, freq);
		std::cout << "idle: " << core.idle << '\n';
		std::cout << "cpu" << corei << ".freq: " << oldfreq << " → "
		          << freq << '\n';
	}
}

void reset_cp_times() {
	for (size_t i = 1; i < g.samples_max; ++i) {
		update_cp_times();
	}
}

} /* namespace */

#include <unistd.h>

int main() {
	try {
		init();
		reset_cp_times();
		while (true) {
			std::this_thread::sleep_for(g.interval);
			update_freq();
		}
	} catch (Exit & e) {
		return static_cast<int>(e);
	}
	return 0;
}


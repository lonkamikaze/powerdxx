/** \file
 * Implements powerd++ a drop in replacement for FreeBSD's powerd.
 */

#include "Options.hpp"

#include <iostream>  /* std::cout, std::cerr */
#include <locale>    /* std::tolower() */
#include <cstdlib>   /* atof() */
#include <memory>    /* std::unique_ptr */
#include <cstdint>   /* uint64_t */
#include <chrono>    /* std::chrono::milliseconds */
#include <thread>    /* std::this_thread::sleep_for() */
#include <algorithm> /* std::min(), std::max() */

#include <sys/types.h>     /* sysctl() */
#include <sys/sysctl.h>    /* sysctl() */
#include <sys/resource.h>  /* CPUSTATES */

#include <signal.h>  /* signal() */
#include <libutil.h> /* pidfile_*() */

namespace {

using namespace powerdxx;

/**
 * Millisecond type for polling intervals.
 */
typedef std::chrono::milliseconds ms;

/**
 * Management Information Base identifier type (see sysctl(3)).
 */
typedef int mib_t;

/**
 * Type for CPU core indexing.
 */
typedef int coreid_t;

/**
 * Type for load counting.
 *
 * According to src/sys/kern/kern_clock.c the type is `long` (an array of
 * loads  `long[CPUSTATES]` is defined).
 * But in order to have defined wrapping characteristics `unsigned long`
 * will be used here.
 */
typedef unsigned long cptime_t;

/**
 * Type for CPU frequencies in MHz.
 */
typedef unsigned int mhz_t;

/**
 * The MIB for hw.ncpu.
 */
mib_t const NCPU_MIB[]{CTL_HW, HW_NCPU};

/**
 * The MIB name for  per-CPU time statistics.
 */
char const CP_TIMES[] = "kern.cp_times";

/**
 * The MIB name for the AC line state.
 */
char const ACLINE[] = "hw.acpi.acline";

/**
 * The available AC line states.
 */
enum class AcLineState : unsigned int {
	BATTERY, ONLINE, UNKNOWN
};

/**
 * String descriptions for the AC line states.
 */
const char * const AcLineStateStr[]{"battery", "online", "unknown"};

/**
 * The MIB name for CPU frequencies.
 */
char const FREQ[] = "dev.cpu.%d.freq";

/**
 * Exit codes.
 */
enum class Exit : int {
	OK, ECLARG, EOUTOFRANGE, ELOAD, EFREQ, EMODE, EIVAL,
	ESAMPLES, ESYSCTL, ENOFREQ, ECONFLICT, EPID, EFORBIDDEN,
	EDAEMON
}; 

/**
 * Printable strings for exit codes.
 */
const char * const ExitStr[]{
	"OK", "ECLARG", "EOUTOFRANGE", "ELOAD", "EFREQ", "EMODE", "EIVAL",
	"ESAMPLES", "ESYSCTL", "ENOFREQ", "ECONFLICT", "EPID", "EFORBIDDEN",
	"EDAEMON"
};

/**
 * Exceptions are an exit code, string message pair.
 */
typedef std::tuple<Exit, std::string> Exception;

/**
 * Like sizeof(), but it returns the number of elements an array consists
 * of instead of the number of bytes.
 *
 * @tparam T,Count
 *	The type and number of array elements
 * @return
 *	The number of array entries
 */
template <typename T, size_t Count>
constexpr size_t countof(T (&)[Count]) { return Count; }

/**
 * Contains the management information for a single CPU core.
 */
struct Core {
	/**
	 * The MIB to kern.cpu.N.freq, if present.
	 */
	mib_t freq_mib[4] = {0};

	/**
	 * The core that controls the frequency for this core.
	 */
	coreid_t controller = -1;

	/**
	 * The load during the last frame, a value in the range [0, 1024].
	 */
	cptime_t load = 0;
};

/**
 * The idle target for adaptive load, equals 50% load.
 */
cptime_t const ADP{512};

/**
 * The idle target for hiadaptive load, equals 37.5% load.
 */
cptime_t const HADP{384};

/**
 * A collection of all the gloabl, mutable states.
 *
 * This is mostly for semantic clarity.
 */
struct {
	/**
	 * The last signal received, used for terminating.
	 */
	int signal{0};

	/**
	 * The number of cp_time samples to take.
	 *
	 * At least 2 are required.
	 */
	size_t samples{5};

	/**
	 * The polling interval.
	 */
	ms interval{500};

	/**
	 * The current sample.
	 */
	size_t sample{0};

	/**
	 * The number of CPU cores or threads.
	 */
	coreid_t ncpu{0};

	/**
	 * Lowest frequency to set in MHz.
	 */
	mhz_t freq_min{0};

	/**
	 * Highest frequency to set in MHz.
	 */
	mhz_t freq_max{1000000};

	/**
	 * Target load times [0, 1024].
	 *
	 * The value 0 indicates the corresponding fixed frequency setting
	 * from target_freqs should be used.
	 */
	cptime_t targets[3]{ADP, HADP, HADP};

	/**
	 * Fixed clock frequencies to use if the target load is set to 0.
	 */
	mhz_t target_freqs[3]{0, 0, 0};

	/**
	 * The target load for the current AC line state.
	 */
	cptime_t target = HADP;

	/**
	 * The target clock for the current AC line state if the target loat
	 * is set to 0.
	 */
	mhz_t target_freq{0};

	/**
	 * The MIB for the AC line.
	 */
	mib_t acline_mib[3];

	/**
	 * The current power source.
	 */
	AcLineState acline{AcLineState::UNKNOWN};

	/**
	 * Verbose/foreground mode.
	 */
	bool verbose{false};

	/**
	 * Name of an alternative pidfile.
	 *
	 * If not given pidfile_open() uses a default name.
	 */
	char const * pidfilename{nullptr};

	/**
	 * The MIB for kern.cp_times.
	 */
	mib_t cp_times_mib[2];

	/**
	 * This is a ring buffer to be allocated with ncpu × samples
	 * instances of cptime_t[CPUSTATES] instances.
	 */
	std::unique_ptr<cptime_t[][CPUSTATES]> cp_times;

	/**
	 * This buffer is to be allocated with ncpu instances of the
	 * Core struct to store the management information of every
	 * core.
	 */
	std::unique_ptr<Core[]> cores;
} g;

static_assert(countof(g.targets) == countof(AcLineStateStr),
              "there must be a target load for each state");

static_assert(countof(g.target_freqs) == countof(AcLineStateStr),
              "there must be a target frequency for each state");

/**
 * Command line argument units.
 *
 * These units are supported for command line arguments, for SCALAR
 * arguments the behaviour of powerd is to be imitated.
 */
enum class Unit : size_t {
	SCALAR, PERCENT, SECOND, MILLISECOND, HZ, KHZ, MHZ, GHZ, THZ, UNKNOWN
};

/**
 * The unit strings on the command line, for the respective Unit instances.
 */
char const * const UnitStr[]{
	"", "%", "s", "ms", "hz", "khz", "mhz", "ghz", "thz"
};

/**
 * A string literal operator equivalent to the `operator "" s` literal
 * provided by C++14 in <string>.
 *
 * @param op
 *	The raw string to turn into an std::string object
 * @param size
 *	The size of the raw string
 * @return
 *	An std::string instance
 */
std::string operator "" _s(char const op[], size_t const size) {
	return {op, size};
}

/**
 * A wrapper around std::snprintf() that automatically pulls in the
 * destination buffer size.
 *
 * @tparam Size
 *	The destination buffer size
 * @tparam Args
 *	The types of the arguments
 * @param dst
 *	A reference to the destination buffer
 * @param format
 *	A printf style formatting string
 * @param args
 *	The printf arguments
 * @return
 *	The number of characters in the resulting string, regardless of the
 *	available space
 */
template <size_t Size, typename... Args>
inline int sprintf(char (&dst)[Size], const char format[],
                   Args const... args) {
	return std::snprintf(dst, Size, format, args...);
}

/**
 * Determine the unit of a string encoded value.
 *
 * @param str
 *	The string to determine the unit of
 * @return
 *	A unit
 */
Unit unit(std::string const & str) {
	size_t pos = 0;
	for (; pos < str.length() && ((str[pos] >= '0' && str[pos] <= '9') ||
	                              str[pos] == '.'); ++pos);
	auto const unitstr = str.substr(pos);
	for (size_t i = 0; i < countof(UnitStr); ++i) {
		if (unitstr == UnitStr[i]) {
			return static_cast<Unit>(i);
		}
	}
	return Unit::UNKNOWN;
}

/**
 * Outputs the given message on stderr if g.verbose is set.
 *
 * @param msg
 *	The message to output
 */
inline void verbose(std::string const & msg) {
	if (g.verbose) {
		std::cerr << "powerd++: " << msg << '\n';
	}
}

/**
 * Throws an Exception instance with the given message.
 *
 * @param exitcode
 *	The exit code to return on termination
 * @param msg
 *	The message to show
 */
inline void fail(Exit const exitcode, std::string const & msg) {
	assert(static_cast<int>(exitcode) < countof(ExitStr) &&
	       "Enum member must have a corresponding string");
	throw Exception{exitcode, "powerd++: ("_s + ExitStr[static_cast<int>(exitcode)] + ") " + msg};
}

/**
 * Treat sysctl errors.
 *
 * Fails appropriately for the given error.
 *
 * @param err
 *	The error number to treat
 */
void sysctl_fail(int const err) {
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

/**
 * Returns the requested sysctl data into the location given by oldp.
 *
 * @tparam Namelen
 *	The length of MIB array
 * @param name
 *	The MIB of the sysctl to return
 * @param oldp,oldplen
 *	Pointer and size to/of the data structure to return the data to
 */
template <u_int Namelen>
void sysctl_get(int const (& name)[Namelen], void * oldp, size_t const oldplen) {
	auto len = oldplen;
	int err = ::sysctl(name, Namelen, oldp, &len, nullptr, 0);
	sysctl_fail(err & errno);
	assert(len == oldplen && "buffer size must match the data returned");
	assert(err == 0 && "untreated error");
}

/**
 * Returns the requested sysctl data into oldv.
 *
 * @tparam Namelen
 *	The length of MIB array
 * @param name
 *	The MIB of the sysctl to return
 * @param oldv
 *	The data to update with the given sysctl
 */
template <u_int Namelen, typename T>
inline void sysctl_get(int const (& name)[Namelen], T & oldv) {
	return sysctl_get(name, &oldv, sizeof(oldv));
}

/**
 * Sets the requested sysctl to the data given by newp.
 *
 * @tparam Namelen
 *	The length of MIB array
 * @param name
 *	The MIB of the sysctl to return
 * @param newp,newplen
 *	Pointer and size to/of the data structure to update the sysctl with
 */
template <u_int Namelen>
void sysctl_set(int const (& name)[Namelen], void const * newp, size_t const newplen) {
	int err = ::sysctl(name, Namelen, nullptr, nullptr, newp, newplen);
	sysctl_fail(err & errno);
	assert(err == 0);
}

/**
 * Sets the requested sysctl to newv.
 *
 * @tparam Namelen
 *	The length of MIB array
 * @param name
 *	The MIB of the sysctl to return
 * @param newv
 *	The data to update the given sysctl with
 */
template <u_int Namelen, typename T>
inline void sysctl_set(int const (& name)[Namelen], T const & newv) {
	return sysctl_set(name, &newv, sizeof(newv));
}

/**
 * Sets a MIB array to the correct MIB for the given sysctl name.
 *
 * @tparam MibLen
 *	The lenght of the MIB array
 * @param name
 *	The name of the sysctl
 * @param mibp
 *	A pointer to the MIB array
 */
template <size_t MibLen>
void sysctlnametomib(const char name[], mib_t (&mibp)[MibLen]) {
	size_t length = MibLen;
	auto err = ::sysctlnametomib(name, mibp, &length);
	sysctl_fail(err & errno);
	assert(MibLen == length &&
	       "The MIB array length should match the returned MIB length");
}

/**
 * Perform initial tasks.
 *
 * - Get number of CPU cores/threads
 * - Determine the clock controlling core for each core
 * - Set the MIBs of hw.acpi.acline and kern.cp_times
 */
void init() {
	/* number of cores */
	sysctl_get(NCPU_MIB, g.ncpu);

	/* get AC line state MIB */
	try {
		sysctlnametomib(ACLINE, g.acline_mib);
	} catch (Exception & e) {
		verbose("cannot read "_s + ACLINE);
	}

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
		} catch (Exception & e) {
			if (std::get<0>(e) != Exit::ESYSCTL) { throw; }

			if (errno == ENOENT) {
				verbose("cannot access "_s + name);
				if (0 > controller) {
					fail(Exit::ENOFREQ, "at least the "
					     "first CPU core must support "
					     "frequency updates");
				}
			} else {
				throw;
			}
		}
		g.cores[core].controller = controller;
	}

	/* MIB for kern.cp_times */
	sysctlnametomib(CP_TIMES, g.cp_times_mib);
	/* create buffer for system load times */
	g.cp_times = std::unique_ptr<cptime_t[][CPUSTATES]>(
	    new cptime_t[g.samples * g.ncpu][CPUSTATES]{{0}});
}

/**
 * Updates the cp_times ring buffer and the load times for each core.
 */
void update_cp_times() {
	sysctl_get(g.cp_times_mib, g.cp_times[g.sample * g.ncpu],
	           g.ncpu * sizeof(g.cp_times[0]));

	for (coreid_t core = 0; core < g.ncpu; ++core) {
		auto const & cp_times = g.cp_times[g.sample * g.ncpu + core];
		auto const & cp_times_old = g.cp_times[((g.sample + 1) % g.samples) * g.ncpu + core];
		uint64_t all = 0;
		for (auto const time : cp_times) { all += time; }
		for (auto const time : cp_times_old) { all -= time; }

		cptime_t idle = cp_times[CP_IDLE] - cp_times_old[CP_IDLE];
		g.cores[core].load = all ? 1024 - cptime_t((uint64_t{idle} << 10) / all) : 0;
	}
	g.sample = (g.sample + 1) % g.samples;
}

/**
 * Sets the load time of each clock controlling core to the maximum load
 * in the group.
 */
void update_load_times() {
	update_cp_times();

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto const & core = g.cores[corei];
		if (core.controller == corei) { continue; }
		auto & controller = g.cores[core.controller];
		controller.load = std::max(controller.load, core.load);
	}
}

/**
 * Update AC line status and fetch the corresponding target load and
 * frequency.
 */
void update_acline() {
	try {
		sysctl_get(g.acline_mib, g.acline);
	} catch (Exception &) {
		g.acline = AcLineState::UNKNOWN;
	}
	int const modei = static_cast<int>(g.acline);
	g.target = g.targets[modei];
	g.target_freq = g.target_freqs[modei];
}

/**
 * Update the CPU clocks depending on the AC line state and targets.
 */
void update_freq() {
	update_load_times();
	update_acline();

	assert(g.target <= 1024 &&
	       "load target must be in the range (0, 1024]");

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto const & core = g.cores[corei];
		if (core.controller != corei) { continue; }

		/* get old clock */
		mhz_t oldfreq = 0;
		if (g.target || g.verbose) {
			sysctl_get(core.freq_mib, oldfreq);
		}
		/* determine target frequency */
		mhz_t freq = 0;
		if (g.target) {
			/* adaptive frequency mode */
			assert(oldfreq == (((oldfreq << 10) & ~0x3ff) >> 10) &&
			       "CPU clock frequency exceeds values that are safe to compute");
			freq = oldfreq * core.load / g.target;
		} else {
			/* fixed frequency mode */
			freq = g.target_freq;
		}
		/* apply limits */
		freq = std::min(std::max(freq, g.freq_min), g.freq_max);
		/* update CPU frequency */
		try {
			sysctl_set(core.freq_mib, freq);
		} catch (Exception & e) {
			if (errno == EPERM) {
				fail(Exit::EFORBIDDEN, "insufficient "
				     "privileges to change core frequency");
			} else {
				throw;
			}
		}
		/* verbose output */
		if (!g.verbose) { continue; }
		std::cout << std::right
		          << "power: " << std::setw(7)
		          << AcLineStateStr[static_cast<int>(g.acline)]
		          << ", load: " << std::setw(3)
		          << ((core.load * 100 + 512) / 1024)
		          << "%, cpu" << corei << ".freq: "
		          << std::setw(4) << oldfreq 
		          << " -> " << std::setw(4) << freq << " MHz\n";
	}
	std::cout << std::flush;
}

/**
 * Fill the cp_times buffer with n - 1 samples.
 */
void reset_cp_times() {
	for (size_t i = 1; i < g.samples; ++i) {
		update_cp_times();
	}
}

/**
 * Convert string to load in the range [0, 1024].
 *
 * The given string must have the following format:
 *
 * \verbatim
 * load = <float>, [ "%" ];
 * \endverbatim
 *
 * The input value must be in the range [0.0, 1.0] or [0%, 100%].
 *
 * @param str
 *	A string encoded load
 * @return
 *	The load given by str
 */
cptime_t load(char const str[]) {
	std::string load{str};
	for (char & ch : load) { ch = std::tolower(ch); }

	auto value = std::atof(str);
	switch (unit(load)) {
	case Unit::SCALAR:
		if (value > 1. || value < 0) {
			fail(Exit::EOUTOFRANGE, "load targets must be in the range [0.0, 1.0]: "_s + str);
		}
		/* convert load to [0, 1024] range */
		value = 1024 * value;
		return value < 1 ? 1 : value;
	case Unit::PERCENT:
		if (value > 100. || value < 0) {
			fail(Exit::EOUTOFRANGE, "load targets must be in the range [0%, 100%]: "_s + str);
		}
		/* convert load to [0, 1024] range */
		value = 1024 * (value / 100.);
		return value < 1 ? 1 : value;
	default:
		break;
	}
	fail(Exit::ELOAD, "load target not recognised: "_s + str);
	return 0;
}

/**
 * Convert string to frequency in MHz.
 *
 * The given string must have the following format:
 *
 * \verbatim
 * freq = <float>, [ "hz" | "khz" | "mhz" | "ghz" | "thz" ];
 * \endverbatim
 *
 * For compatibility with powerd MHz are assumed, if no unit string is given.
 *
 * The resulting frequency must be in the range [0Hz, 1THz].
 *
 * @param str
 *	A string encoded frequency
 * @return
 *	The frequency given by str
 */
mhz_t freq(char const str[]) {
	std::string freqstr{str};
	for (char & ch : freqstr) { ch = std::tolower(ch); }

	auto value = std::atof(str);
	switch (unit(freqstr)) {
	case Unit::HZ:
		value /= 1000000.;
		goto unit__mhz;
	case Unit::KHZ:
		value /= 1000.;
		goto unit__mhz;
	case Unit::SCALAR: /* for compatibilty with powerd */
	case Unit::MHZ:
	unit__mhz:
		if (value > 1000000. || value < 0) {
			fail(Exit::EOUTOFRANGE, "target frequency must be in the range [0Hz, 1THz]: "_s + str);
		}
		return mhz_t(value);
	case Unit::GHZ:
		value *= 1000.;
		goto unit__mhz;
	case Unit::THZ:
		value *= 1000000.;
		goto unit__mhz;
	default:
		break;
	}
	fail(Exit::EFREQ, "frequency value not recognised: "_s + str);
	return 0;
};

/**
 * Sets a load target or fixed frequency for the given AC line state.
 *
 * The string must be in the following format:
 *
 * \verbatim
 * mode_predefined = "minimum" | "min" | "maximum" | "max" |
 *                   "adaptive" | "adp" | "hiadptive" | "hadp";
 * mode =            mode_predefined | load | freq;
 * \endverbatim
 *
 * Scalar values are treated as loads.
 *
 * The predefined values have the following meaning:
 *
 * | Symbol     | Meaning                                      |
 * |------------|----------------------------------------------|
 * | minimum    | The minimum clock rate (default 0 MHz)       |
 * | min        |                                              |
 * | maximum    | The maximum clock rate (default 1000000 MHz) |
 * | max        |                                              |
 * | adaptive   | A target load of 50%                         |
 * | adp        |                                              |
 * | hiadptive  | A target load of 25%                         |
 * | hadp       |                                              |
 *
 * @param line
 *	The power line state to set the mode for
 * @param str
 *	A mode string
 */
void set_mode(AcLineState const line, char const str[]) {
	std::string mode{str};
	for (char & ch : mode) { ch = std::tolower(ch); }
	
	auto const linei = static_cast<unsigned int>(line);
	g.targets[linei] = 0;
	g.target_freqs[linei] = 0;

	if (mode == "minimum" || mode == "min") {
		g.target_freqs[linei] = 0;
		return;
	}
	if (mode == "maximum" || mode == "max") {
		g.target_freqs[linei] = 1000000; return;
	}
	if (mode == "adaptive" || mode == "adp") {
		g.targets[linei] = ADP;
		return;
	}
	if (mode == "hiadaptive" || mode == "hadp") {
		g.targets[linei] = HADP;
		return;
	}

	/* try to set load,
	 * do that first so it gets the scalar values */
	try {
		g.targets[linei] = load(str);
		return;
	} catch (Exception & e) {
		if (std::get<0>(e) == Exit::EOUTOFRANGE) { throw; }
	}

	/* try to set clock frequency */
	try {
		g.target_freqs[linei] = freq(str);
		return;
	} catch (Exception & e) {
		if (std::get<0>(e) == Exit::EOUTOFRANGE) { throw; }
	}

	fail(Exit::EMODE, "mode not recognised: "_s + str);
}

/**
 * Convert string to time interval in milliseconds.
 *
 * The given string must have the following format:
 *
 * \verbatim
 * ival = <float>, [ "s" | "ms" ];
 * \endverbatim
 *
 * For compatibility with powerd scalar values are assumed to represent
 * milliseconds.
 *
 * @param str
 *	A string encoded time interval
 * @return
 *	The interval in milliseconds
 */
ms ival(char const str[]) {
	std::string interval{str};
	for (char & ch : interval) { ch = std::tolower(ch); }

	auto value = std::atof(str);
	if (value < 0) {
		fail(Exit::EOUTOFRANGE, "polling interval must be positive: "_s + str);
		return ms{0};
	}
	switch (unit(interval)) {
	case Unit::SECOND:
		return ms{static_cast<long long>(value * 1000.)};
	case Unit::SCALAR: /* for powerd compatibility */
	case Unit::MILLISECOND:
		return ms{static_cast<long long>(value)};
	default:
		break;
	}
	fail(Exit::EIVAL, "polling interval not recognised: "_s + str);
	return ms{0};
}

/**
 * A string encoded number of samples.
 *
 * The string is expected to contain a scalar integer.
 *
 * @param str
 *	The string containing the number of samples
 * @return
 *	The number of samples
 */
size_t samples(char const str[]) {
	if (unit(str) != Unit::SCALAR) {
		fail(Exit::ESAMPLES, "sample count must be a scalar integer: "_s + str);
	}
	auto const cnt = std::atoi(str);
	auto const cntf = std::atof(str);
	if (cntf != cnt) {
		fail(Exit::EOUTOFRANGE, "sample count must be an integer: "_s + str);
	}
	if (cnt < 2 || cnt > 1001) {
		fail(Exit::EOUTOFRANGE, "sample count must be in the range [2; 1001]: "_s + str);
	}
	return size_t(cnt);
}

/**
 * An enum for command line parsing.
 */
enum class OE {
	USAGE, MODE_AC, MODE_BATT, LOAD_IDLE, FREQ_MIN, FREQ_MAX,
	MODE_UNKNOWN, IVAL_POLL, FILE_PID, LOAD_RUN, FLAG_VERBOSE,
	CNT_SAMPLES,
	/* obligatory: */ OPT_UNKNOWN, OPT_NOOPT, OPT_DASH, OPT_LDASH, OPT_DONE
};

/**
 * The short usage string.
 */
char const USAGE[] = "[-hv] [-abn mode] [-mM freq] [-p ival] [-s cnt] [-P file]";

/**
 * Definitions of command line options.
 */
Option<OE> const OPTIONS[]{
	{OE::USAGE,        'h', "help",    "",     "Show usage and exit"},
	{OE::FLAG_VERBOSE, 'v', "verbose", "",     "Be verbose, stay in foreground"},
	{OE::MODE_AC,      'a', "ac",      "mode", "Select the mode while on AC power"},
	{OE::MODE_BATT,    'b', "batt",    "mode", "Select the mode while on battery power"},
	{OE::MODE_UNKNOWN, 'n', "unknown", "mode", "Select the mode while power source is unknown"},
	{OE::FREQ_MIN,     'm', "min",     "freq", "The minimum CPU frequency"},
	{OE::FREQ_MAX,     'M', "max",     "freq", "The maximum CPU frequency"},
	{OE::IVAL_POLL,    'p', "poll",    "ival", "The polling interval"},
	{OE::CNT_SAMPLES,  's', "samples", "cnt",  "The number of samples to use"},
	{OE::FILE_PID,     'P', "pid",     "file", "Alternative PID file"},
	{OE::LOAD_IDLE,    'i', "idle",    "load", "Ignored"},
	{OE::LOAD_RUN,     'r', "run",     "load", "Ignored"}
};

/**
 * Parse command line arguments.
 *
 * @param argc,argv
 *	The command line arguments
 */
void read_args(int const argc, char const * const argv[]) {
	auto getopt = make_Options(argc, argv, USAGE, OPTIONS);

	while (true) switch (getopt()) {
	case OE::USAGE:
		throw Exception{Exit::OK, getopt.usage()};
		break;
	case OE::FLAG_VERBOSE:
		g.verbose = true;
		break;
	case OE::MODE_AC:
		set_mode(AcLineState::ONLINE, getopt[1]);
		break;
	case OE::MODE_BATT:
		set_mode(AcLineState::BATTERY, getopt[1]);
		break;
	case OE::MODE_UNKNOWN:
		set_mode(AcLineState::UNKNOWN, getopt[1]);
		break;
	case OE::FREQ_MIN:
		g.freq_min = freq(getopt[1]);
		break;
	case OE::FREQ_MAX:
		g.freq_max = freq(getopt[1]);
		break;
	case OE::IVAL_POLL:
		g.interval = ival(getopt[1]);
		break;
	case OE::CNT_SAMPLES:
		g.samples = samples(getopt[1]);
		break;
	case OE::FILE_PID:
		g.pidfilename = getopt[1];
		break;
	case OE::LOAD_IDLE:
	case OE::LOAD_RUN:
		/* for compatibility with powerd, ignore */
		break;
	case OE::OPT_UNKNOWN:
	case OE::OPT_NOOPT:
	case OE::OPT_DASH:
	case OE::OPT_LDASH:
		fail(Exit::ECLARG, "unexpected command line argument: "_s +
		                   getopt[0] + "\n\n" + getopt.usage());
		return;
	case OE::OPT_DONE:
		return;
	}
}

/**
 * Prints the configuration on stderr in verbose mode.
 */
void show_settings() {
	if (!g.verbose) {
		return;
	}
	std::cerr << "Load Sampling\n"
	          << "\tcp_time samples:       " << g.samples << '\n'
	          << "\tpolling interval:      " << g.interval.count() << " ms\n"
	          << "\tload average over:     " << ((g.samples - 1) *
	                                             g.interval.count()) << " ms\n"
	          << "CPU Cores\n"
	          << "\tCPU cores:             " << g.ncpu << '\n'
	          << "\tfrequency limits:      [" << g.freq_min << " MHz, "
	                                      << g.freq_max << " MHz]\n"
	          << "Core Groups\n";
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		if (i == g.cores[i].controller) {
			if (i > 0) {
				std::cerr << (i - 1) << "]\n";
			}
			std::cerr << '\t' << i << ": [" << i << ", ";
		}
	}
	std::cerr << (g.ncpu - 1) << "]\n"
	          << "Load Targets\n";
	for (size_t i = 0; i < countof(g.targets); ++i) {
		std::cerr << '\t' << std::left << std::setw(23)
		          << (""_s + AcLineStateStr[i] + " power target:")
		          << (g.targets[i] ?
		              ((g.targets[i] * 100 + 512) / 1024) : g.target_freqs[i])
		          << (g.targets[i] ? "% load\n" : " MHz\n");
	}
}

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

	/**
	 * The errno value of the last call to a pidfile_*() function.
	 */
	int err;

	public:
	/**
	 * Attempts to open the pidfile.
	 *
	 * @param pfname,mode
	 *	Arguments to pidfile_open()
	 */
	Pidfile(char const pfname[], mode_t const mode) :
	    otherpid{0}, pfh{::pidfile_open(pfname, mode, &this->otherpid)},
	    err{this->pfh == nullptr ? errno : 0} {}

	/**
	 * Removes the pidfile.
	 */
	~Pidfile() {
		::pidfile_remove(this->pfh);
	}

	/**
	 * Returns the last error.
	 *
	 * @return
	 *	Returns the errno of the last pidfile_*() command if an
	 *	error occured
	 */
	int error() { return this->err; }

	/**
	 * Returns the PID of the other process holding the lock.
	 */
	pid_t other() { return this->otherpid; }

	/**
	 * Write PID to the file, should be called after daemon().
	 */
	void write() {
		this->err = ::pidfile_write(this->pfh);
		this->err &= errno;
	}
};

/**
 * Daemonise and run the main loop.
 */
void run_daemon() {
	/* open pidfile */
	Pidfile pidfile{g.pidfilename, 0600};
	switch (pidfile.error()) {
	case 0:
		break;
	case EEXIST:
		fail(Exit::ECONFLICT, "already running under PID: "_s +
		                      std::to_string(pidfile.other()));
		return;
	default:
		fail(Exit::EPID, "cannot create pidfile "_s +
		                 (g.pidfilename ? g.pidfilename : ""));
		return;
	}

	/* one chance to fail before forking */
	update_freq();

	/* daemonise */
	if (!g.verbose && -1 == ::daemon(0, 1)) {
		fail(Exit::EDAEMON, "detaching the process failed");
	}

	/* write pid */
	pidfile.write();
	if (pidfile.error()){
		fail(Exit::EPID, "cannot write to pidfile "_s +
		                 (g.pidfilename ? g.pidfilename : ""));
	}

	/* the main loop */
	while (g.signal == 0) {
		std::this_thread::sleep_for(g.interval);
		update_freq();
	}

	verbose("signal "_s + std::to_string(g.signal) + " received, exiting ...");
}

/**
 * Sets g.signal, terminating the main loop.
 */
void signal_recv(int const signal) {
	g.signal = signal;
}

} /* namespace */

/**
 * Main routine, setup and execute daemon, print errors.
 *
 * @param argc,argv
 *	The command line arguments
 * @return
 *	An exit code
 * @see Exit
 */
int main(int const argc, char const * const argv[]) {
	try {
		signal(SIGINT, signal_recv);
		signal(SIGTERM, signal_recv);
		read_args(argc, argv);
		init();
		show_settings();
		reset_cp_times();
		run_daemon();
	} catch (Exception & e) {
		if (std::get<1>(e) != "") {
			std::cerr << std::get<1>(e) << '\n';
		}
		return static_cast<int>(std::get<0>(e));
	}
	return 0;
}


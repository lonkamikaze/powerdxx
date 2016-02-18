
#include "Options.hpp"

#include <iostream>
#include <locale>  /* std::tolower() */
#include <cstdlib> /* atof() */
#include <memory>  /* std::unique_ptr */
#include <cstdint> /* uint64_t */
#include <chrono>  /* std::chrono::milliseconds */
#include <thread>  /* std::this_thread::sleep_for() */
#include <limits>  /* std::numeric_limits */

#include <sys/types.h>     /* sysctl() */
#include <sys/sysctl.h>    /* sysctl() */
#include <sys/resource.h>  /* CPUSTATES */

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
typedef unsigned int freq_t;

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
	OK, ESYSCTL, ENOFREQ, EFORBIDDEN, EMODE, ECLARG, EFREQ,
	EIVAL, ESAMPLES
}; 

/**
 * Printable strings for exit codes.
 */
const char * const ExitStr[]{
	"OK", "ESYSCTL", "ENOFREQ", "EFORBIDDEN", "EMODE", "ECLARG", "EFREQ",
	"EIVAL", "ESAMPLES"
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

struct Core {
	mib_t freq_mib[4] = {0};
	coreid_t controller = -1;
	cptime_t idle = 0;
};

cptime_t const ADP{512};
cptime_t const HADP{768};

/* wrap all the mutable globals in a struct for clearer semantics */
struct {
	size_t samples_max = 5;
	size_t sample = 0;
	cptime_t target = ADP;
	freq_t target_freq{0};
	coreid_t ncpu;
	ms interval{500};
	freq_t freq_min{0};
	freq_t freq_max{1000000};

	cptime_t targets[3]{ADP, HADP, HADP};
	freq_t target_freqs[3]{0, 0, 0};

	mib_t acline_mib[3];
	AcLineState acline = AcLineState::UNKNOWN;

	bool verbose = false;

	mib_t cp_times_mib[2];
	std::unique_ptr<cptime_t[][CPUSTATES]> cp_times;

	std::unique_ptr<Core[]> cores;
} g;

static_assert(countof(g.targets) == countof(AcLineStateStr),
              "there must be a target idle load for each state");

static_assert(countof(g.target_freqs) == countof(AcLineStateStr),
              "there must be a target frequency for each state");

enum class Unit : size_t {
	SCALAR, PERCENT, SECOND, MILLISECOND, HZ, KHZ, MHZ, GHZ, THZ, UNKNOWN
};

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
inline int sprintf(char (&dst)[Size], const char * format,
                   Args const... args) {
	return std::snprintf(dst, Size, format, args...);
}

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

inline void verbose(std::string const & msg) {
	if (g.verbose) {
		std::cerr << "powerd++: " << msg << '\n';
	}
}

inline void fail(Exit const exitcode, std::string const & msg) {
	assert(static_cast<int>(exitcode) < countof(ExitStr) &&
	       "Enum member must have a corresponding string");
	throw Exception{exitcode, "powerd++: ("_s + ExitStr[static_cast<int>(exitcode)] + ") " + msg};
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
	assert(len == oldplen && "buffer size must match the data returned");
	assert(err == 0 && "untreated error");
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
void sysctlnametomib(const char name[], mib_t (&mibp)[MibLen]) {
	size_t length = MibLen;
	auto err = ::sysctlnametomib(name, mibp, &length);
	sysctl_fail(err & errno);
	assert(MibLen == length &&
	       "The MIB array length should match the returned MIB length");
}

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
		g.cores[core].idle = all ? cptime_t((uint64_t{idle} << 10) / all) : 1024;
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

void update_freq() {
	update_idle_times();
	update_acline();

	assert(g.target <= 1024 &&
	       "idle target must be in the range (0, 1024]");

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto const & core = g.cores[corei];
		if (core.controller != corei) { continue; }

		/* determine target frequency */
		freq_t freq = 0;
		if (g.target) {
			/* adaptive frequency mode */
			sysctl_get(core.freq_mib, freq);
			assert(freq == (((freq << 10) & ~0x3ff) >> 10) &&
			       "CPU frequency exceeds values that are safe to compute");
			freq = freq * g.target / (core.idle ? core.idle : 1);
		} else {
			/* fixed frequency mode */
			freq = g.target_freq;
		}
		/* apply limits */
		if (freq < g.freq_min) {
			freq = g.freq_min;
		} else if (freq > g.freq_max) {
			freq = g.freq_max;
		}
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
		if (g.verbose) {
			sysctl_get(core.freq_mib, freq);
			std::cout << "power: " << AcLineStateStr[static_cast<int>(g.acline)]
			          << " load: " << ((1024 - core.idle) * 100 / 1024)
			          << "% cpu" << corei << ".freq: " << freq << '\n';
		}
	}
}

void reset_cp_times() {
	for (size_t i = 1; i < g.samples_max; ++i) {
		update_cp_times();
	}
}

void set_mode(AcLineState const line, char const str[]) {
	std::string mode{str};
	for (char & ch : mode) { ch = std::tolower(ch); }
	
	auto const linei = static_cast<unsigned int>(line);

	if (mode == "maximum" || mode == "max") {
		g.targets[linei] = 0;
		g.target_freqs[linei] = 1000000;
		return;
	}
	if (mode == "minimum" || mode == "min") {
		g.targets[linei] = 0;
		g.target_freqs[linei] = 0;
		return;
	}
	if (mode == "adaptive" || mode == "adp") {
		g.targets[linei] = ADP;
		return;
	}
	if (mode == "hiadaptive" || mode == "hadp") {
		g.targets[linei] = HADP;
		return;
	}
	auto value = std::atof(str);
	switch (unit(mode)) {
	case Unit::SCALAR:
		if (value > 1. || value < 0) {
			return fail(Exit::EMODE, "Load targets must be in the range [0.0, 1.0]");
		}
		value = 1024 * (1. - value); /* convert load to idle value */
		g.targets[linei] = value < 1 ? 1 : value;
		return;
	case Unit::PERCENT:
		if (value > 100. || value < 0) {
			return fail(Exit::EMODE, "Load targets must be in the range [0%, 100%]");
		}
		value = 1024 * (1. - (value / 100.)); /* convert load to idle value */
		g.targets[linei] = value < 1 ? 1 : value;
		return;
	case Unit::HZ:
		value /= 1000000.;
		goto unit__mhz;
	case Unit::KHZ:
		value /= 1000.;
		goto unit__mhz;
	case Unit::MHZ:
	unit__mhz:
		if (value > 1000000. || value < 0) {
			return fail(Exit::EMODE, "Target frequency must be in the range [0Hz, 1THz]");
		}
		g.targets[linei] = 0;
		g.target_freqs[linei] = freq_t(value);
		return;
	case Unit::GHZ:
		value *= 1000.;
		goto unit__mhz;
	case Unit::THZ:
		value *= 1000000.;
		goto unit__mhz;
	default:
		break;
	}
	fail(Exit::EMODE, "Mode '"_s + mode + "' not recognised");
}

freq_t freq(char const str[]) {
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
			fail(Exit::EFREQ, "Target frequency must be in the range [0Hz, 1THz]");
			return 0;
		}
		return freq_t(value);
	case Unit::GHZ:
		value *= 1000.;
		goto unit__mhz;
	case Unit::THZ:
		value *= 1000000.;
		goto unit__mhz;
	default:
		break;
	}
	fail(Exit::EFREQ, "Frequency value not recognised: "_s + str);
	return 0;
};

ms ival(char const * const str) {
	std::string interval{str};
	for (char & ch : interval) { ch = std::tolower(ch); }

	auto value = std::atof(str);
	if (value < 0) {
		fail(Exit::EIVAL, "Polling interval must be positive: "_s + str);
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
	fail(Exit::EIVAL, "Polling interval not recognised: "_s + str);
	return ms{0};
}

size_t samples(char const str[]) {
	if (unit(str) != Unit::SCALAR) {
		fail(Exit::ESAMPLES, "Sample count must be a number: "_s + str);
	}
	auto const cnt = std::atoi(str);
	auto const cntf = std::atof(str);
	if (cntf != cnt) {
		fail(Exit::ESAMPLES, "Sample count must be an integer: "_s + str);
	}
	if (cnt < 2 || cnt > 1001) {
		fail(Exit::ESAMPLES, "Sample count must be in the range [2; 1001]: "_s + str);
	}
	return size_t(cnt);
}

enum class OE {
	USAGE, MODE_AC, MODE_BATT, LOAD_IDLE, FREQ_MIN, FREQ_MAX,
	MODE_UNKNOWN, IVAL_POLL, FILE_PID, LOAD_RUN, FLAG_VERBOSE,
	CNT_SAMPLES,
	/* obligatory: */ OPT_UNKNOWN, OPT_NOOPT, OPT_DASH, OPT_LDASH, OPT_DONE
};

char const USAGE[] = "[-hv] [-abn mode] [-mM freq] [-p ival] [-s cnt] [-P pid]";

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
		g.samples_max = samples(getopt[1]);
		break;
	case OE::FILE_PID:
		//TODO
		break;
	case OE::LOAD_IDLE:
	case OE::LOAD_RUN:
		/* for compatibility with powerd, ignore */
		break;
	case OE::OPT_UNKNOWN:
	case OE::OPT_NOOPT:
	case OE::OPT_DASH:
	case OE::OPT_LDASH:
		fail(Exit::ECLARG, "Unexpected command line argument: "_s +
		                   getopt[0] + "\n\n" + getopt.usage());
		return;
	case OE::OPT_DONE:
		return;
	}
}

} /* namespace */

int main(int const argc, char const * const argv[]) {
	try {
		read_args(argc, argv);
		init();
		reset_cp_times();
		while (true) {
			std::this_thread::sleep_for(g.interval);
			update_freq();
		}
	} catch (Exception & e) {
		std::cerr << std::get<1>(e) << '\n';
		return static_cast<int>(std::get<0>(e));
	}
	return 0;
}


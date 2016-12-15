/** \file
 * Implements powerd++ a drop in replacement for FreeBSD's powerd.
 */

#include "Options.hpp"

#include "types.hpp"
#include "constants.hpp"
#include "errors.hpp"
#include "clas.hpp"
#include "utility.hpp"

#include "sys/sysctl.hpp"
#include "sys/pidfile.hpp"

#include <iostream>  /**< std::cout, std::cerr */
#include <locale>    /**< std::tolower() */
#include <memory>    /**< std::unique_ptr */
#include <chrono>    /**< std::chrono::steady_clock::now() */
#include <thread>    /**< std::this_thread::sleep_until(), std::thread */
#include <algorithm> /**< std::min(), std::max() */

#include <cstdlib>   /**< atof(), atoi(), strtol() */
#include <cstdint>   /**< uint64_t */

#include <sys/resource.h>  /**< CPUSTATES */

#include <signal.h>  /**< signal() */

/**
 * File local scope.
 */
namespace {

using nih::Option;
using nih::make_Options;

using types::cptime_t;
using types::mhz_t;
using types::coreid_t;
using types::ms;

using errors::Exit;
using errors::Exception;
using errors::fail;

using clas::load;
using clas::freq;
using clas::ival;
using clas::samples;

using utility::countof;
using utility::sprintf;
using utility::to_value;
using namespace utility::literals;

using constants::CP_TIMES;
using constants::ACLINE;
using constants::FREQ;
using constants::FREQ_LEVELS;

using constants::FREQ_DEFAULT_MAX;
using constants::FREQ_DEFAULT_MIN;
using constants::FREQ_UNSET;
using constants::POWERD_PIDFILE;
using constants::ADP;
using constants::HADP;

using sys::ctl::make_Once;

/**
 * The available AC line states.
 */
enum class AcLineState : unsigned int {
	BATTERY, /**< Battery is power source */
	ONLINE,  /**< External power source */
	UNKNOWN  /**< Unknown power source */
};

/**
 * String descriptions for the AC line states.
 */
char const * const AcLineStateStr[]{"battery", "online", "unknown"};

/**
 * Contains the management information for a single CPU core.
 */
struct Core {
	/**
	 * The sysctl kern.cpu.N.freq, if present.
	 */
	sys::ctl::SysctlSync<mhz_t, 4> freq{{}};

	/**
	 * A pointer to the kern.cp_times section for this core.
	 */
	cptime_t const * cp_time;

	/**
	 * The kern.cpu.N.freq value for the current load sample.
	 *
	 * This is updated by update_loads().
	 */
	mhz_t sample_freq{0};

	/**
	 * The core that controls the frequency for this core.
	 */
	coreid_t controller{-1};

	/**
	 * The idle ticks count.
	 */
	cptime_t idle{0};

	/**
	 * Count of all ticks.
	 */
	cptime_t all{0};

	/**
	 * For the controlling core this is set to the group loadsum.
	 *
	 * This is reset by update_loads() and set by update_group_loads().
	 */
	mhz_t group_loadsum{0};

	/**
	 * The sum of all load samples.
	 *
	 * This is updated by update_loads().
	 */
	mhz_t loadsum{0};

	/**
	 * A ring buffer of load samples for this core.
	 *
	 * Each load sample is weighted with the core frequency at
	 * which it was taken.
	 *
	 * This is updated by update_loads().
	 */
	std::unique_ptr<mhz_t[]> loads;

	/**
	 * The minimum core clock rate.
	 */
	mhz_t min{FREQ_DEFAULT_MIN};

	/**
	 * The maximum core clock rate.
	 */
	mhz_t max{FREQ_DEFAULT_MAX};
};



/**
 * A collection of all the gloabl, mutable states.
 *
 * This is mostly for semantic clarity.
 */
struct {
	/**
	 * The last signal received, used for terminating.
	 */
	volatile int signal{0};

	/**
	 * The number of load samples to take.
	 */
	size_t samples{4};

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
	sys::ctl::SysctlOnce<coreid_t, 2> const ncpu{1, {CTL_HW, HW_NCPU}};

	/**
	 * Per AC line state settings.
	 */
	struct {
		/**
		 * Lowest frequency to set in MHz.
		 */
		mhz_t freq_min;

		/**
		 * Highest frequency to set in MHz.
		 */
		mhz_t freq_max;

		/**
		 * Target load times [0, 1024].
		 *
		 * The value 0 indicates the corresponding fixed frequency setting
		 * from target_freqs should be used.
		 */
		cptime_t target_load;

		/**
		 * Fixed clock frequencies to use if the target load is set to 0.
		 */
		mhz_t target_freq;
	} acstates[3] {
		{FREQ_UNSET,       FREQ_UNSET,       ADP,  0},
		{FREQ_UNSET,       FREQ_UNSET,       HADP, 0},
		{FREQ_DEFAULT_MIN, FREQ_DEFAULT_MAX, HADP, 0}
	};

	/**
	 * The hw.acpi.acline sysctl.
	 */
	sys::ctl::Sysctl<3> acline_ctl{};

	/**
	 * Verbose mode.
	 */
	bool verbose{false};

	/**
	 * Foreground mode.
	 */
	bool foreground{false};

	/**
	 * Name of an alternative pidfile.
	 *
	 * If not given pidfile_open() uses a default name.
	 */
	char const * pidfilename{POWERD_PIDFILE};

	/**
	 * The kern.cp_times sysctl.
	 */
	sys::ctl::Sysctl<2> cp_times_ctl{};

	/**
	 * The kern.cp_times buffer for all cores.
	 */
	std::unique_ptr<cptime_t[][CPUSTATES]> cp_times;

	/**
	 * This buffer is to be allocated with ncpu instances of the
	 * Core struct to store the management information of every
	 * core.
	 */
	std::unique_ptr<Core[]> cores;
} g;

static_assert(countof(g.acstates) == countof(AcLineStateStr),
              "There must be a configuration tuple for each state");

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
 * Treat sysctl errors.
 *
 * Fails appropriately for the given error.
 *
 * @param err
 *	The errno value after calling sysctl
 */
[[noreturn]] inline
void sysctl_fail(sys::sc_error<sys::ctl::error> const err) {
	fail(Exit::ESYSCTL, err, "sysctl failed: "_s + err.c_str());
}

/**
 * Perform initial tasks.
 *
 * - Get number of CPU cores/threads
 * - Determine the clock controlling core for each core
 * - Set the MIBs of hw.acpi.acline and kern.cp_times
 */
void init() {
	/* get AC line state MIB */
	try {
		g.acline_ctl = {ACLINE};
	} catch (sys::sc_error<sys::ctl::error>) {
		verbose("cannot read "_s + ACLINE);
	}

	/*
	 * Get the frequency controlling core for each core.
	 * Basically acts as if the kernel supported local frequency changes.
	 */
	g.cores = std::unique_ptr<Core[]>(new Core[g.ncpu]);
	coreid_t controller = -1;
	for (coreid_t core = 0; core < g.ncpu; ++core) {
		/* get the frequency handler */
		char name[40];
		sprintf(name, FREQ, core);
		try {
			g.cores[core].freq = {{name}};
			controller = core;
		} catch (sys::sc_error<sys::ctl::error> e) {
			verbose("cannot access sysctl: "_s + name);
			if (e == ENOENT) {
				if (0 > controller) {
					fail(Exit::ENOFREQ, e, "at least the first CPU core must support frequency updates");
				}
			} else {
				sysctl_fail(e);
			}
		}
		g.cores[core].controller = controller;

		/* create loads buffer */
		g.cores[core].loads = std::unique_ptr<mhz_t[]>{new mhz_t[g.samples]{}};
	}

	/* set user frequency boundaries */
	auto const line_unknown = to_value(AcLineState::UNKNOWN);
	for (auto & state : g.acstates) {
		if (state.freq_min == FREQ_UNSET) {
			state.freq_min = g.acstates[line_unknown].freq_min;
		}
		if (state.freq_max == FREQ_UNSET) {
			state.freq_max = g.acstates[line_unknown].freq_max;
		}
	}

	/* set per core min/max frequency boundaries */
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		auto & core = g.cores[i];
		if (core.controller != i) { continue; }
		char name[40];
		sprintf(name, FREQ_LEVELS, i);
		try {
			sys::ctl::Sysctl<4> const ctl{name};
			auto levels = ctl.get<char>();
			/* the maximum should at least be the minimum
			 * and vice versa */
			core.max = FREQ_DEFAULT_MIN;
			core.min = FREQ_DEFAULT_MAX;
			for (auto pch = levels.get(); *pch; ++pch) {
				mhz_t freq = strtol(pch, &pch, 10);
				if (pch[0] != '/') { break; }
				core.max = std::max(core.max, freq);
				core.min = std::min(core.min, freq);
				strtol(++pch, &pch, 10);
				/* no idea what that value means */
				if (pch[0] != ' ') { break; }
			}
			assert(core.min <= core.max &&
			       "minimum must not be greater than maximum");
		} catch (sys::sc_error<sys::ctl::error>) {
			verbose("cannot access sysctl: "_s + name);
		}
	}

	/* MIB for kern.cp_times */
	g.cp_times_ctl = {CP_TIMES};

	/* create buffer for system load ticks */
	g.cp_times = std::unique_ptr<cptime_t[][CPUSTATES]>{
		new cptime_t[g.ncpu][CPUSTATES]{}};
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		auto & core = g.cores[i];
		core.cp_time = g.cp_times[i];
	}
}

/**
 * Updates the cp_times ring buffer and computes the load average for
 * each core.
 */
void update_loads() {
	/* update load ticks */
	try {
		g.cp_times_ctl.get(g.cp_times[0],
		                   g.ncpu * sizeof(g.cp_times[0]));
	} catch (sys::sc_error<sys::ctl::error> e) {
		sysctl_fail(e);
	}

	mhz_t freq = 0;
	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto & core = g.cores[corei];

		/* reset controling core data */
		if (core.controller == corei) {
			core.sample_freq = core.freq;
			freq = core.sample_freq;
			core.group_loadsum = 0;
		}

		/* sum of collected ticks */
		cptime_t all_new = 0;
		for (size_t i = 0; i < CPUSTATES; ++i) {
			all_new += core.cp_time[i];
		}
		cptime_t const all = all_new - core.all;
		core.all = all_new;

		/* collected idle ticks */
		cptime_t idle_new = core.cp_time[CP_IDLE];
		cptime_t const idle = idle_new - core.idle;
		core.idle = idle_new;

		/* subtract oldest sample */
		core.loadsum -= core.loads[g.sample];
		/* update current sample */
		if (all) {
			/* measurement succeeded */
			core.loads[g.sample] = freq - (freq * idle) / all;
		} else {
			/* no samples since last sampling, reuse the previous
			 * load */
			core.loads[g.sample] =
			    core.loads[(g.sample + g.samples - 1) % g.samples];
		}
		/* add current sample */
		core.loadsum += core.loads[g.sample];
	}
	g.sample = (g.sample + 1) % g.samples;
}

/**
 * Sets the load time of each clock controlling core to the maximum load
 * in the group.
 */
void update_group_loads() {
	update_loads();

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto const & core = g.cores[corei];
		auto & controller = g.cores[core.controller];
		controller.group_loadsum = std::max(controller.group_loadsum,
		                                    core.loadsum);
	}
}

/**
 * Update the CPU clocks depending on the AC line state and targets.
 */
void update_freq() {
	update_group_loads();

	/* get AC line status */
	auto const acline = to_value<AcLineState>(
	    make_Once(AcLineState::UNKNOWN, g.acline_ctl));
	auto const & acstate = g.acstates[acline];

	assert(acstate.target_load <= 1024 &&
	       "load target must be in the range [0, 1024]");

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto & core = g.cores[corei];
		if (core.controller != corei) { continue; }

		/* determine target frequency */
		mhz_t wantfreq = 0;
		if (acstate.target_load) {
			/* adaptive frequency mode */
			wantfreq = core.group_loadsum / g.samples *
			           1024 / acstate.target_load;
		} else {
			/* fixed frequency mode */
			wantfreq = acstate.target_freq;
		}
		/* apply limits */
		auto const max = std::min(core.max, acstate.freq_max);
		auto const min = std::max(core.min, acstate.freq_min);
		mhz_t newfreq = std::min(std::max(wantfreq, min), max);
		/* update CPU frequency */
		if (core.sample_freq != newfreq) {
			core.freq = newfreq;
		}
		/* verbose output */
		if (!g.foreground) { continue; }
		std::cout << "power: %7s, load: %4d MHz, cpu%d.freq: %4d MHz, wanted: %4d MHz\n"_fmt
		             (AcLineStateStr[acline],
		              (core.group_loadsum / g.samples),
		              corei, core.sample_freq, wantfreq);
	}
	if (g.foreground) { std::cout << std::flush; }
}

/**
 * Fill the loads buffers with n samples.
 *
 * The samples are filled with the target load, this creates a bias
 * to stay at the initial frequency until sufficient real measurements
 * come in to flush these initial samples out.
 */
void init_loads() {
	/* call it once to initialise its internal state */
	update_loads();

	/* get AC line status */
	auto const acline = to_value<AcLineState>(
	    make_Once(AcLineState::UNKNOWN, g.acline_ctl));
	auto const & acstate = g.acstates[acline];

	/* fill the load buffer for each core */
	mhz_t load = 0;
	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto & core = g.cores[corei];

		/* recalculate target load for controlling cores */
		if (core.controller == corei) {
			load = core.freq * acstate.target_load / 1024;
		}

		/* apply target load to the whole sample buffer */
		for (size_t i = 0; i < g.samples; ++i) {
			core.loadsum -= core.loads[i];
			core.loadsum += load;
			core.loads[i] = load;
		}
	}
}

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
 * | hiadptive  | A target load of 37.5%                       |
 * | hadp       |                                              |
 *
 * @param line
 *	The power line state to set the mode for
 * @param str
 *	A mode string
 */
void set_mode(AcLineState const line, char const * const str) {
	std::string mode{str};
	for (char & ch : mode) { ch = std::tolower(ch); }

	auto const acline = to_value(line);
	auto & acstate = g.acstates[acline];

	acstate.target_load = 0;
	acstate.target_freq = 0;

	if (mode == "minimum" || mode == "min") {
		acstate.target_freq = FREQ_DEFAULT_MIN;
		return;
	}
	if (mode == "maximum" || mode == "max") {
		acstate.target_freq = FREQ_DEFAULT_MAX;
		return;
	}
	if (mode == "adaptive" || mode == "adp") {
		acstate.target_load = ADP;
		return;
	}
	if (mode == "hiadaptive" || mode == "hadp") {
		acstate.target_load = HADP;
		return;
	}

	/* try to set load,
	 * do that first so it gets the scalar values */
	try {
		acstate.target_load = load(str);
		return;
	} catch (Exception & e) {
		if (e.exitcode == Exit::EOUTOFRANGE) { throw; }
	}

	/* try to set clock frequency */
	try {
		acstate.target_freq = freq(str);
		return;
	} catch (Exception & e) {
		if (e.exitcode == Exit::EOUTOFRANGE) { throw; }
	}

	fail(Exit::EMODE, 0, "mode not recognised: "_s + str);
}

/**
 * An enum for command line parsing.
 */
enum class OE {
	USAGE,           /**< Print help */
	MODE_AC,         /**< Set AC power mode */
	MODE_BATT,       /**< Set battery power mode */
	FREQ_MIN,        /**< Set minimum clock frequency */
	FREQ_MAX,        /**< Set maximum clock frequency */
	FREQ_MIN_AC,     /**< Set minimum clock frequency on AC power */
	FREQ_MAX_AC,     /**< Set maximum clock frequency on AC power */
	FREQ_MIN_BATT,   /**< Set minimum clock frequency on battery power */
	FREQ_MAX_BATT,   /**< Set maximum clock frequency on battery power */
	MODE_UNKNOWN,    /**< Set unknown power source mode */
	IVAL_POLL,       /**< Set polling interval */
	FILE_PID,        /**< Set pidfile */
	FLAG_VERBOSE,    /**< Activate verbose output on stderr */
	FLAG_FOREGROUND, /**< Stay in foreground, log events to stdout */
	CNT_SAMPLES,     /**< Set number of load samples */
	IGNORE,          /**< Legacy settings */
	OPT_UNKNOWN,     /**< Obligatory */
	OPT_NOOPT,       /**< Obligatory */
	OPT_DASH,        /**< Obligatory */
	OPT_LDASH,       /**< Obligatory */
	OPT_DONE         /**< Obligatory */
};

/**
 * The short usage string.
 */
char const * const USAGE = "[-hvf] [-abn mode] [-mM freq] [-p ival] [-s cnt] [-P file]";

/**
 * Definitions of command line options.
 */
Option<OE> const OPTIONS[]{
	{OE::USAGE,           'h', "help",       "",     "Show usage and exit"},
	{OE::FLAG_VERBOSE,    'v', "verbose",    "",     "Be verbose"},
	{OE::FLAG_FOREGROUND, 'f', "foreground", "",     "Stay in foreground"},
	{OE::MODE_AC,         'a', "ac",         "mode", "Select the mode while on AC power"},
	{OE::MODE_BATT,       'b', "batt",       "mode", "Select the mode while on battery power"},
	{OE::MODE_UNKNOWN,    'n', "unknown",    "mode", "Select the mode while power source is unknown"},
	{OE::FREQ_MIN,        'm', "min",        "freq", "The minimum CPU frequency"},
	{OE::FREQ_MAX,        'M', "max",        "freq", "The maximum CPU frequency"},
	{OE::FREQ_MIN_AC,      0 , "min-ac",     "freq", "The minimum CPU frequency on AC power"},
	{OE::FREQ_MAX_AC,      0 , "max-ac",     "freq", "The maximum CPU frequency on AC power"},
	{OE::FREQ_MIN_BATT,    0 , "min-batt",   "freq", "The minimum CPU frequency on battery power"},
	{OE::FREQ_MAX_BATT,    0 , "max-batt",   "freq", "The maximum CPU frequency on battery power"},
	{OE::IVAL_POLL,       'p', "poll",       "ival", "The polling interval"},
	{OE::CNT_SAMPLES,     's', "samples",    "cnt",  "The number of samples to use"},
	{OE::FILE_PID,        'P', "pid",        "file", "Alternative PID file"},
	{OE::IGNORE,          'i', "",           "load", "Ignored"},
	{OE::IGNORE,          'r', "",           "load", "Ignored"}
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
		std::cerr << getopt.usage();
		throw Exception{Exit::OK, 0, ""};
	case OE::FLAG_VERBOSE:
		g.verbose = true;
		break;
	case OE::FLAG_FOREGROUND:
		g.foreground = true;
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
		g.acstates[to_value(AcLineState::UNKNOWN)]
		    .freq_min = freq(getopt[1]);
		break;
	case OE::FREQ_MAX:
		g.acstates[to_value(AcLineState::UNKNOWN)]
		    .freq_max = freq(getopt[1]);
		break;
	case OE::FREQ_MIN_AC:
		g.acstates[to_value(AcLineState::ONLINE)]
		    .freq_min = freq(getopt[1]);
		break;
	case OE::FREQ_MAX_AC:
		g.acstates[to_value(AcLineState::ONLINE)]
		    .freq_max = freq(getopt[1]);
		break;
	case OE::FREQ_MIN_BATT:
		g.acstates[to_value(AcLineState::BATTERY)]
		    .freq_min = freq(getopt[1]);
		break;
	case OE::FREQ_MAX_BATT:
		g.acstates[to_value(AcLineState::BATTERY)]
		    .freq_max = freq(getopt[1]);
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
	case OE::IGNORE:
		/* for compatibility with powerd, ignore */
		break;
	case OE::OPT_UNKNOWN:
	case OE::OPT_NOOPT:
	case OE::OPT_DASH:
	case OE::OPT_LDASH:
		fail(Exit::ECLARG, 0, "unexpected command line argument: "_s +
		                      getopt[0] + "\n\n" + getopt.usage());
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
	std::cerr << "Terminal Output\n"
	             "\tverbose:               yes\n"
	             "\tforeground:            %s\n"
	             "Load Sampling\n"
	             "\tload samples:          %d\n"
	             "\tpolling interval:      %d ms\n"
	             "\tload average over:     %d ms\n"
	             "Frequency Limits\n"_fmt
	             (g.foreground ? "yes" : "no",
	              g.samples, g.interval.count(),
	              g.samples * g.interval.count());
	for (size_t i = 0; i < countof(g.acstates); ++i) {
		std::cerr << "\t%-22s [%d MHz, %d MHz]\n"_fmt
		             ((""_s + AcLineStateStr[i] + ':').c_str(),
		              g.acstates[i].freq_min, g.acstates[i].freq_max);
	}
	std::cerr << "CPU Cores\n"
	             "\tCPU cores:             %d\n"
	             "Core Groups\n"_fmt(g.ncpu);
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		if (i == g.cores[i].controller) {
			if (i > 0) {
				std::cerr << (i - 1) << "]\n";
			}
			std::cerr << '\t' << i << ": [" << i << ", ";
		}
	}
	std::cerr << (g.ncpu - 1) << "]\n"
	          << "Core Frequency Limits\n";
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		if (i != g.cores[i].controller) { continue; }
		std::cerr << "\t%d: [%d MHz, %d MHz]\n"_fmt
		             (i, g.cores[i].min, g.cores[i].max);
	}
	std::cerr << "Load Targets\n";
	for (size_t i = 0; i < countof(g.acstates); ++i) {
		auto const & acstate = g.acstates[i];
		std::cerr << "\t%-22s %2d%% load\n"_fmt
		             ((""_s + AcLineStateStr[i] + " power target:").c_str(),
		              acstate.target_load
		              ? (acstate.target_load * 100 + 512) / 1024
		              : acstate.target_freq,
		              acstate.target_load);
	}
}

/**
 * A core frequency guard.
 *
 * This uses the RAII pattern to achieve two things:
 *
 * - Upon creation it reads and writes all controlling cores
 * - Upon destruction it sets all cores to the maximum frequencies
 */
class FreqGuard final {
	private:
	/**
	 * The list of initial frequencies.
	 */
	std::unique_ptr<mhz_t[]> freqs;

	public:
	/**
	 * Read and write all core frequencies, may throw.
	 */
	FreqGuard() : freqs{new mhz_t[g.ncpu]} {
		for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
			auto & core = g.cores[corei];
			if (core.controller != corei) { continue; }
			try {
				/* remember clock frequency */
				this->freqs[corei] = core.freq;
				/* attempt clock frequency write */
				core.freq = this->freqs[corei];
			} catch (sys::sc_error<sys::ctl::error> e) {
				if (EPERM == e) {
					fail(Exit::EFORBIDDEN, e,
					     "insufficient privileges to change core frequency");
				} else {
					sysctl_fail(e);
				}
			}
		}
	}

	/**
	 * Restore all core frequencies.
	 */
	~FreqGuard() {
		for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
			auto & core = g.cores[corei];
			if (core.controller != corei) { continue; }
			try {
				core.freq = this->freqs[corei];
			} catch (sys::sc_error<sys::ctl::error>) {
				/* do nada */
			}
		}
	}
};

/**
 * Sets g.signal, terminating the main loop.
 *
 * @param signal
 *	The signal number received
 */
void signal_recv(int const signal) {
	g.signal = signal;
}

/**
 * Daemonise and run the main loop.
 */
void run_daemon() try {
	/* open pidfile */
	sys::pid::Pidfile pidfile{g.pidfilename, 0600};

	/* try to set frequencies once, before detaching from the terminal */
	FreqGuard fguard;

	/* detach from the terminal */
	if (!g.foreground && -1 == ::daemon(0, 1)) {
		fail(Exit::EDAEMON, errno, "detaching the process failed");
	}

	/* Setup SIGHUP */
	if (g.foreground) {
		/* Terminate in foreground */
		signal(SIGHUP, signal_recv);
	} else {
		/* Ignore in daemon mode */
		signal(SIGHUP, SIG_IGN);
	}

	/* write pid */
	try {
		pidfile.write();
	} catch (sys::sc_error<sys::pid::error> e) {
		fail(Exit::EPID, e,
		     "cannot write to pidfile: "_s + g.pidfilename);
	}

	/* the main loop */
	auto time = std::chrono::steady_clock::now();
	while (g.signal == 0) {
		std::this_thread::sleep_until(time += g.interval);
		update_freq();
	}

	verbose("signal %d received, exiting ..."_fmt(g.signal));
} catch (pid_t otherpid) {
	fail(Exit::ECONFLICT, EEXIST,
	     "a power daemon is already running under PID: %d"_fmt(otherpid));
} catch (sys::sc_error<sys::pid::error> e) {
	fail(Exit::EPID, e,
	     "cannot create pidfile: "_s + g.pidfilename);
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
int main(int argc, char * argv[]) {
	try {
		signal(SIGINT, signal_recv);
		signal(SIGTERM, signal_recv);
		read_args(argc, argv);
		init();
		show_settings();
		init_loads();
		run_daemon();
	} catch (Exception & e) {
		if (e.msg != "") {
			std::cerr << "powerd++: " << e.msg << '\n';
		}
		return to_value(e.exitcode);
	} catch (sys::sc_error<sys::ctl::error> e) {
		std::cerr << "powerd++: untreated sysctl failure: " << e.c_str() << '\n';
		throw;
	} catch (sys::sc_error<sys::pid::error> e) {
		std::cerr << "powerd++: untreated pidfile failure: " << e.c_str() << '\n';
		throw;
	} catch (...) {
		std::cerr << "powerd++: untreated failure\n";
		throw;
	}
	return 0;
}


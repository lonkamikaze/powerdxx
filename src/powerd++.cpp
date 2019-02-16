/** \file
 * Implements powerd++ a drop in replacement for FreeBSD's powerd.
 */

#include "Options.hpp"
#include "Cycle.hpp"

#include "types.hpp"
#include "constants.hpp"
#include "errors.hpp"
#include "clas.hpp"
#include "utility.hpp"

#include "sys/sysctl.hpp"
#include "sys/pidfile.hpp"
#include "sys/signal.hpp"

#include <iostream>  /* std::cout, std::cerr */
#include <locale>    /* std::tolower() */
#include <memory>    /* std::unique_ptr */
#include <algorithm> /* std::min(), std::max() */
#include <limits>    /* std::numeric_limits */

#include <cstdlib>   /* strtol() */
#include <cstdint>   /* uint64_t */

#include <sys/resource.h>  /* CPUSTATES */

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
using types::decikelvin_t;

using errors::Exit;
using errors::Exception;
using errors::fail;

using clas::load;
using clas::freq;
using clas::ival;
using clas::samples;
using clas::temperature;
using clas::celsius;
using clas::range;

using utility::countof;
using utility::sprintf_safe;
using utility::to_value;
using namespace utility::literals;
using utility::Min;
using utility::Max;

using constants::CP_TIMES;
using constants::ACLINE;
using constants::FREQ;
using constants::FREQ_LEVELS;
using constants::TEMPERATURE;
using constants::TJMAX_SOURCES;

using constants::FREQ_DEFAULT_MAX;
using constants::FREQ_DEFAULT_MIN;
using constants::FREQ_UNSET;
using constants::POWERD_PIDFILE;
using constants::ADP;
using constants::HADP;
using constants::HITEMP_OFFSET;

/**
 * The available AC line states.
 */
enum class AcLineState : unsigned int {
	BATTERY, /**< Battery is power source */
	ONLINE,  /**< External power source */
	UNKNOWN, /**< Unknown power source */
	LENGTH   /**< Enum length */
};

/**
 * Contains the management information for a group of cores with
 * a common clock frequency.
 */
struct CoreGroup {
	/**
	 * The sysctl dev.cpu.%d.freq.
	 */
	sys::ctl::SysctlSync<mhz_t> freq{{}};

	/**
	 * The number of the core owning dev.cpu.%d.freq.
	 */
	coreid_t corei{0};

	/**
	 * The dev.cpu.%d.freq value for the current load sample.
	 *
	 * This is updated by update_loads().
	 */
	mhz_t sample_freq{0};

	/**
	 * The minimum group clock rate.
	 *
	 * The greatest of all core minima in the group.
	 */
	Max<mhz_t> min{FREQ_DEFAULT_MIN};

	/**
	 * The maximum group clock rate.
	 *
	 * The least of all core maxima in the group.
	 */
	Min<mhz_t> max{FREQ_DEFAULT_MAX};

	/**
	 * The maximum load reported by all cores in the group.
	 *
	 * This is updated by update_loads().
	 */
	Max<mhz_t> load{0};

	/**
	 * A ring buffer of maximum load samples for this core group.
	 *
	 * Each maximum load sample is weighted with the core frequency at
	 * which it was taken.
	 *
	 * This is updated by update_loads().
	 */
	std::unique_ptr<mhz_t[]> loads;

	/**
	 * The maximum load sum of all controlled cores.
	 *
	 * This is updated by update_loads().
	 */
	mhz_t loadsum{0};

	/**
	 * Critical core temperature in dK.
	 */
	Min<decikelvin_t> temp_crit{std::numeric_limits<decikelvin_t>::max()};

	/**
	 * High core temperature in dK.
	 */
	Min<decikelvin_t> temp_high{std::numeric_limits<decikelvin_t>::max()};

	/**
	 * The maximum temperature measurement taken in the group.
	 */
	Max<decikelvin_t> temp{0};
};

/**
 * Contains the management information for a single CPU core.
 */
struct Core {
	/**
	 * A pointer to the kern.cp_times section for this core.
	 */
	cptime_t const * cp_time;

	/**
	 * The core that controls the frequency for this core.
	 */
	CoreGroup * group{nullptr};

	/**
	 * The idle ticks count.
	 */
	cptime_t idle{0};

	/**
	 * Count of all ticks.
	 */
	cptime_t all{0};

	/**
	 * The dev.cpu.%d.temperature sysctl, if present.
	 */
	sys::ctl::SysctlSync<decikelvin_t> temp{{}};
};

/**
 * A collection of all the gloabl, mutable states.
 *
 * This is mostly for semantic clarity.
 */
struct Global {
	/**
	 * The last signal received, used for terminating.
	 */
	volatile sig_atomic_t signal{0};

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
	struct ACSet {
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

		/**
		 * The string representation of this state.
		 */
		char const * const name;
	} acstates[3] {
		{FREQ_UNSET,       FREQ_UNSET,       ADP,  0, "battery"},
		{FREQ_UNSET,       FREQ_UNSET,       HADP, 0, "online"},
		{FREQ_DEFAULT_MIN, FREQ_DEFAULT_MAX, HADP, 0, "unknown"}
	}; /**< The power states. */

	/**
	 * The hw.acpi.acline ctl.
	 */
	sys::ctl::Sysctl<> acline_ctl;

	/**
	 * Verbose mode.
	 */
	bool verbose{false};

	/**
	 * Foreground mode.
	 */
	bool foreground{false};

	/**
	 * Temperature throttling mode.
	 */
	bool temp_throttling{false};

	/**
	 * User set critical core temperature in dK.
	 */
	decikelvin_t temp_crit{0};

	/**
	 * User set high core temperature in dK.
	 */
	decikelvin_t temp_high{0};

	/**
	 * Name of an alternative pidfile.
	 *
	 * If not given pidfile_open() uses a default name.
	 */
	char const * pidfilename{POWERD_PIDFILE};

	/**
	 * The kern.cp_times sysctl.
	 */
	sys::ctl::Sysctl<> cp_times_ctl{};

	/**
	 * The kern.cp_times buffer for all cores.
	 */
	std::unique_ptr<cptime_t[][CPUSTATES]> cp_times;

	/**
	 * This buffer is to be allocated with ncpu instances of the
	 * Core struct to store the management information of every
	 * core.
	 */
	std::unique_ptr<Core[]> cores{new Core[this->ncpu]};

	/**
	 * The number of frequency controlling core groups.
	 */
	coreid_t ngroups{0};

	/**
	 * This buffer is to be allocated with the number of core
	 * groups. A core group is created by init() for each core
	 * that has a dev.cpu.%d.freq handle.
	 */
	std::unique_ptr<CoreGroup[]> groups{nullptr};
} g; /**< The gobal state. */

static_assert(countof(g.acstates) == to_value(AcLineState::LENGTH),
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
	 * Count the number of controlling cores and set up the core
	 * group buffer.
	 */
	for (coreid_t core = 0; core < g.ncpu; ++core) {
		/* get the frequency handler */
		char name[40];
		sprintf_safe(name, FREQ, core);
		try {
			sys::ctl::Sysctl<>{name};
			++g.ngroups;
		} catch (sys::sc_error<sys::ctl::error> e) {
		}
	}
	g.groups = std::unique_ptr<CoreGroup[]>{new CoreGroup[g.ngroups]{}};

	/*
	 * Get the frequency controlling core for each core.
	 * Basically acts as if the kernel supported local frequency changes.
	 */
	coreid_t groupi = -1;
	for (coreid_t core = 0; core < g.ncpu; ++core) {
		/* get the frequency handler and setup loads buffers */
		char name[40];
		sprintf_safe(name, FREQ, core);
		try {
			sys::ctl::Sysctl<> const ctl{name};
			auto & group = g.groups[++groupi];
			group.freq = {ctl};
			group.corei = core;
			/* create loads buffer */
			group.loads = std::unique_ptr<mhz_t[]>{new mhz_t[g.samples]{}};
		} catch (sys::sc_error<sys::ctl::error> e) {
			if (e == ENOENT) {
				if (0 > groupi) {
					fail(Exit::ENOFREQ, e, "cannot access "_s + name + ", at least the first CPU core must support frequency updates");
				}
			} else {
				verbose("cannot access sysctl: "_s + name);
				sysctl_fail(e);
			}
		}
		g.cores[core].group = &g.groups[groupi];
	}

	/* set user frequency boundaries */
	auto const & line_unknown = g.acstates[to_value(AcLineState::UNKNOWN)];
	for (auto & state : g.acstates) {
		if (state.freq_min == FREQ_UNSET) {
			state.freq_min = line_unknown.freq_min;
		}
		if (state.freq_max == FREQ_UNSET) {
			state.freq_max = line_unknown.freq_max;
		}
		/* check user frequency boundaries */
		if (state.freq_min < state.freq_max) { continue; }
		fail(Exit::EOUTOFRANGE, 0,
		     "frequency limits 'min < max' violation:\n"
		     "\t%s [%d MHz, %d MHz]"_fmt
		     (state.name, state.freq_min, state.freq_max));
	}

	/* setup temperature throttling */
	if (g.temp_throttling) {
		/* user provided throttling values */

		/* check temperature throttling boundaries */
		if (g.temp_high >= g.temp_crit) {
			fail(Exit::EOUTOFRANGE, 0,
			     "temperature throttling 'high < critical' violation:\n"
			     "\t[%d C, %d C]"_fmt
			     (celsius(g.temp_high), celsius(g.temp_crit)));
		}

		/* propagate limits to all core groups */
		assert(g.groups);
		for (coreid_t i = 0; i < g.ngroups; ++i) {
			g.groups[i].temp_high = g.temp_high;
			g.groups[i].temp_crit = g.temp_crit;
		}
	} else {
		/* try to determine tjmax */
		for (coreid_t core = 0; core < g.ncpu; ++core) {
			assert(g.cores[core].group);
			auto & group = *g.cores[core].group;
			for (auto const source : TJMAX_SOURCES) {
				char name[40];
				sprintf_safe(name, source, core);
				try {
					group.temp_crit =
					    sys::ctl::make_Once<decikelvin_t>
					        (group.temp_crit,
					         sys::ctl::Sysctl<>{name});
					g.temp_throttling = true;
					group.temp_high =
					    group.temp_crit - HITEMP_OFFSET;
					break;
				} catch (sys::sc_error<sys::ctl::error>) {
					/* do nada */
				}
			}
		}
	}
	if (!g.temp_throttling) {
		verbose("could not determine critical temperature\n"
		        "\ttemperature throttling: off");
	} else for (coreid_t i = 0; i < g.ncpu; ++i) {
		char name[40];
		sprintf_safe(name, TEMPERATURE, i);
		try {
			g.cores[i].temp = {{name}};
			assert(g.cores[i].temp >= 0);
		} catch (sys::sc_error<sys::ctl::error>) {
			verbose("core temperature not accessible: %s"_fmt(name));
		}
	}

	/* set per group min/max frequency boundaries */
	CoreGroup * group = nullptr;
	for (coreid_t i = 0; i < g.ncpu; ++i) {
		auto & core = g.cores[i];
		if (core.group == group) { continue; }
		group = core.group;
		char name[40];
		sprintf_safe(name, FREQ_LEVELS, i);
		try {
			sys::ctl::Sysctl<> const ctl{name};
			auto levels = ctl.get<char>();
			/* the maximum should at least be the minimum
			 * and vice versa */
			Max<mhz_t> max{FREQ_DEFAULT_MIN};
			Min<mhz_t> min{FREQ_DEFAULT_MAX};
			for (auto pch = levels.get(); *pch; ++pch) {
				mhz_t freq = strtol(pch, &pch, 10);
				if (pch[0] != '/') { break; }
				max = freq;
				min = freq;
				strtol(++pch, &pch, 10);
				/* no idea what that value means */
				if (pch[0] != ' ') { break; }
			}
			assert(min < max &&
			       "minimum must be less than maximum");
			group->min = min;
			group->max = max;
		} catch (sys::sc_error<sys::ctl::error>) {
			if (g.temp_throttling) {
				verbose("cannot access sysctl: "_s + name +
				        "\n\ttemperature throttling: off");
				g.temp_throttling = false;
			} else {
				verbose("cannot access sysctl: "_s + name);
			}
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

	/* test kern.cp_times is readable */
	try {
		g.cp_times_ctl.get(g.cp_times[0],
		                   g.ncpu * sizeof(g.cp_times[0]));
	} catch (sys::sc_error<sys::ctl::error> e) {
		/* The kern.cp_times sysctl must be readable, ENOMEM
		 * is  fine, see update_loads(). */
		if (e != ENOMEM) {
			sysctl_fail(e);
		}
	}
}

/**
 * Updates the cp_times ring buffer and computes the load average for
 * each core.
 *
 * @tparam Load
 *	Determines whether CoreGroup::loadsum is updated
 * @tparam Temperature
 *	Determines whether CoreGroup::temp is updated
 */
template <bool Load = 1, bool Temperature = 0>
void update_loads() {
	/* update load ticks */
	if (Load) try {
		g.cp_times_ctl.get(g.cp_times[0],
		                   g.ncpu * sizeof(g.cp_times[0]));
	} catch (sys::sc_error<sys::ctl::error> e) {
		/*
		 * Ignore errors assuming it's ENOMEM.
		 *
		 * If HT is disabled kern.cp_times reports more cores
		 * than hw.ncpu does. It's fine to ignore, because
		 * these excess cores never report any load.
		 *
		 * The init() function performs a test read to ensure
		 * kern.cp_times does not fail for a different reason.
		 */
	}

	assert(g.groups);
	for (coreid_t groupi = 0; groupi < g.ngroups; ++groupi) {
		/* reset controlling core data */
		auto & group = g.groups[groupi];
		group.sample_freq = group.freq;
		Temperature && (group.temp = Max<decikelvin_t>{0});
	}

	for (coreid_t corei = 0; corei < g.ncpu; ++corei) {
		auto & core = g.cores[corei];
		assert(core.group);
		auto & group = *core.group;

		/* update load */
		if (Load) {
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

			/* update current sample */
			mhz_t const freq = group.sample_freq;
			if (all) {
				/* measurement succeeded */
				group.load = freq - (freq * idle) / all;
			} else {
				/*
				 * just hope another core in the group
				 * reports something
				 */
			}
		}

		/* update group temperature */
		if (Temperature) try {
			group.temp = core.temp;
		} catch (sys::sc_error<sys::ctl::error> e) {
			verbose("access to core %d temperature failed"_fmt(corei));
			if (g.temp_throttling) {
				verbose("turn off temperature based throttling");
				g.temp_throttling = false;
			}
		}
	}

	for (coreid_t groupi = 0; Load && groupi < g.ngroups; ++groupi) {
		auto & group = g.groups[groupi];
		/* subtract oldest sample */
		group.loadsum -= group.loads[g.sample];
		/* update current sample */
		group.loads[g.sample] = group.load;
		/* add current sample */
		group.loadsum += group.loads[g.sample];
		/* reset current group load for next cycle */
		group.load = Max<mhz_t>{0};
	}

	Load && (g.sample = (g.sample + 1) % g.samples);
}

/**
 * Do nada if neither load nor temperature are to be updated.
 */
template <> void update_loads<0, 0>() {}

/**
 * Update the CPU clocks depending on the AC line state and targets.
 *
 * @tparam Foreground
 *	Set for foreground operation (reporting on std::cout)
 * @tparam Temperature
 *	Set for temperature based throttling
 * @tparam Fixed
 *	Set for fixed frequency mode
 * @param acstate
 *	The set of acline dependent variables
 */
template <bool Foreground, bool Temperature, bool Fixed>
void update_freq(Global::ACSet const & acstate) {
	update_loads<(!Fixed || Foreground), Temperature>();

	assert(g.groups);
	for (coreid_t groupi = 0; groupi < g.ngroups; ++groupi) {
		auto & group = g.groups[groupi];

		/* determine target frequency */
		auto const max = std::min<mhz_t>(group.max, acstate.freq_max);
		auto const min = std::max<mhz_t>(group.min, acstate.freq_min);
		mhz_t wantfreq{0};
		if (!Fixed) {
			/* adaptive frequency mode */
			wantfreq = group.loadsum / g.samples *
			           1024 / acstate.target_load;
		} else {
			/* fixed frequency mode */
			/*
			 * Do not distinguish between newfreq and
			 * wantfreq in this mode, because users might
			 * be disturbed by seeing a 1THz target.
			 */
			wantfreq = acstate.target_freq;
		}
		Min<mhz_t> newfreq{max};
		newfreq = std::max(min, wantfreq);
		/* apply temperature throttling */
		if (Temperature) {
			if (group.temp >= group.temp_crit) {
				newfreq = group.min;
			} else if (group.temp > group.temp_high) {
				auto const tempdiff  = group.temp_crit - group.temp;
				auto const temprange = group.temp_crit - group.temp_high;
				mhz_t const tempfreq = group.max * tempdiff / temprange;
				newfreq = std::max<mhz_t>(tempfreq, group.min);
			}
		}
		/* update CPU frequency */
		if (group.sample_freq != newfreq) {
			group.freq = newfreq;
		}
		/* foreground output */
		if (Foreground && Temperature) {
			std::cout << "power: %7s, load: %4d MHz, %3d C, cpu.%d.freq: %4d MHz, wanted: %4d MHz\n"_fmt
			             (acstate.name,
			              (group.loadsum / g.samples),
			              celsius(group.temp), group.corei,
			              group.sample_freq, wantfreq);
		} else if (Foreground) {
			std::cout << "power: %7s, load: %4d MHz, cpu.%d.freq: %4d MHz, wanted: %4d MHz\n"_fmt
			             (acstate.name,
			              (group.loadsum / g.samples), group.corei,
			              group.sample_freq, wantfreq);
		}
	}
	if (Foreground) { std::cout << std::flush; }
}

/**
 * Dispatch update_freq<>().
 */
void update_freq() {
	/* get AC line status */
	auto const acline = to_value<AcLineState>(
	    sys::ctl::make_Once(AcLineState::UNKNOWN, g.acline_ctl));
	auto const & acstate = g.acstates[acline];

	assert(acstate.target_load <= 1024 &&
	       "load target must be in the range [0, 1024]");

	switch ((g.foreground << 2) | (g.temp_throttling << 1) |
	        (acstate.target_load == 0)) {
	case 0b000:
		return update_freq<0, 0, 0>(acstate);
	case 0b001:
		return update_freq<0, 0, 1>(acstate);
	case 0b010:
		return update_freq<0, 1, 0>(acstate);
	case 0b011:
		return update_freq<0, 1, 1>(acstate);
	case 0b100:
		return update_freq<1, 0, 0>(acstate);
	case 0b101:
		return update_freq<1, 0, 1>(acstate);
	case 0b110:
		return update_freq<1, 1, 0>(acstate);
	case 0b111:
		return update_freq<1, 1, 1>(acstate);
	}

	assert(false && "update_freq<>() was not dispatched");
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
	    sys::ctl::make_Once(AcLineState::UNKNOWN, g.acline_ctl));
	auto const & acstate = g.acstates[acline];

	/* fill the load buffer for each core */
	mhz_t load = 0;
	assert(g.groups);
	for (coreid_t groupi = 0; groupi < g.ngroups; ++groupi) {
		auto & group = g.groups[groupi];

		/* recalculate target load for controlling groups */
		load = group.sample_freq * acstate.target_load / 1024;

		/* apply target load to the whole sample buffer */
		for (size_t i = 0; i < g.samples; ++i) {
			group.loadsum -= group.loads[i];
			group.loadsum += load;
			group.loads[i] = load;
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
	FREQ_RANGE,      /**< Set clock frequency range */
	FREQ_RANGE_AC,   /**< Set clock frequency range on AC power */
	FREQ_RANGE_BATT, /**< Set clock frequency range on battery power */
	HITEMP_RANGE,    /**< Set a high temperature range */
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
char const * const USAGE = "[-hvf] [-abn mode] [-mM freq] [-FAB freq:freq] [-H temp:temp] [-p ival] [-s cnt] [-P file]";

/**
 * Definitions of command line options.
 */
Option<OE> const OPTIONS[]{
	{OE::USAGE,           'h', "help",            "",          "Show usage and exit"},
	{OE::FLAG_VERBOSE,    'v', "verbose",         "",          "Be verbose"},
	{OE::FLAG_FOREGROUND, 'f', "foreground",      "",          "Stay in foreground"},
	{OE::MODE_AC,         'a', "ac",              "mode",      "Mode while on AC power"},
	{OE::MODE_BATT,       'b', "batt",            "mode",      "Mode while on battery power"},
	{OE::MODE_UNKNOWN,    'n', "unknown",         "mode",      "Mode while power source is unknown"},
	{OE::FREQ_MIN,        'm', "min",             "freq",      "Minimum CPU frequency"},
	{OE::FREQ_MAX,        'M', "max",             "freq",      "Maximum CPU frequency"},
	{OE::FREQ_MIN_AC,      0 , "min-ac",          "freq",      "Minimum CPU frequency on AC power"},
	{OE::FREQ_MAX_AC,      0 , "max-ac",          "freq",      "Maximum CPU frequency on AC power"},
	{OE::FREQ_MIN_BATT,    0 , "min-batt",        "freq",      "Minimum CPU frequency on battery power"},
	{OE::FREQ_MAX_BATT,    0 , "max-batt",        "freq",      "Maximum CPU frequency on battery power"},
	{OE::FREQ_RANGE,      'F', "freq-range",      "freq:freq", "CPU frequency range (min:max)"},
	{OE::FREQ_RANGE_AC,   'A', "freq-range-ac",   "freq:freq", "CPU frequency range on AC power"},
	{OE::FREQ_RANGE_BATT, 'B', "freq-range-batt", "freq:freq", "CPU frequency range on battery power"},
	{OE::HITEMP_RANGE,    'H', "hitemp-range",    "temp:temp", "High temperature range (high:critical)"},
	{OE::IVAL_POLL,       'p', "poll",            "ival",      "The polling interval"},
	{OE::CNT_SAMPLES,     's', "samples",         "cnt",       "The number of samples to use"},
	{OE::FILE_PID,        'P', "pid",             "file",      "Alternative PID file"},
	{OE::IGNORE,          'i', "",                "load",      "Ignored"},
	{OE::IGNORE,          'r', "",                "load",      "Ignored"}
};

/**
 * Parse command line arguments.
 *
 * @param argc,argv
 *	The command line arguments
 */
void read_args(int const argc, char const * const argv[]) {
	auto getopt = make_Options(argc, argv, USAGE, OPTIONS);

	auto & ac_on = g.acstates[to_value(AcLineState::ONLINE)];
	auto & ac_batt = g.acstates[to_value(AcLineState::BATTERY)];
	auto & ac_unknown = g.acstates[to_value(AcLineState::UNKNOWN)];

	try {
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
			ac_unknown.freq_min = freq(getopt[1]);
			break;
		case OE::FREQ_MAX:
			ac_unknown.freq_max = freq(getopt[1]);
			break;
		case OE::FREQ_MIN_AC:
			ac_on.freq_min = freq(getopt[1]);
			break;
		case OE::FREQ_MAX_AC:
			ac_on.freq_max = freq(getopt[1]);
			break;
		case OE::FREQ_MIN_BATT:
			ac_batt.freq_min = freq(getopt[1]);
			break;
		case OE::FREQ_MAX_BATT:
			ac_batt.freq_max = freq(getopt[1]);
			break;
		case OE::FREQ_RANGE:
			std::tie(ac_unknown.freq_min, ac_unknown.freq_max) =
			    range(freq, getopt[1]);
			break;
		case OE::FREQ_RANGE_AC:
			std::tie(ac_on.freq_min, ac_on.freq_max) =
			    range(freq, getopt[1]);
			break;
		case OE::FREQ_RANGE_BATT:
			std::tie(ac_batt.freq_min, ac_batt.freq_max) =
			    range(freq, getopt[1]);
			break;
		case OE::HITEMP_RANGE:
			g.temp_throttling = true;
			std::tie(g.temp_high, g.temp_crit) =
			    range(temperature, getopt[1]);
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
			fail(Exit::ECLARG, 0,
			     "unexpected command line argument: "_s + getopt[0]);
		case OE::OPT_DONE:
			return;
		}
	} catch (Exception & e) {
		switch (getopt) {
		case OE::USAGE:
			break;
		case OE::FLAG_VERBOSE:
		case OE::FLAG_FOREGROUND:
			e.msg += "\n\n";
			e.msg += getopt.show(0);
			break;
		case OE::MODE_AC:
		case OE::MODE_BATT:
		case OE::MODE_UNKNOWN:
		case OE::FREQ_MIN:
		case OE::FREQ_MAX:
		case OE::FREQ_MIN_AC:
		case OE::FREQ_MAX_AC:
		case OE::FREQ_MIN_BATT:
		case OE::FREQ_MAX_BATT:
		case OE::FREQ_RANGE:
		case OE::FREQ_RANGE_AC:
		case OE::FREQ_RANGE_BATT:
		case OE::HITEMP_RANGE:
		case OE::IVAL_POLL:
		case OE::CNT_SAMPLES:
		case OE::FILE_PID:
		case OE::IGNORE:
			e.msg += "\n\n";
			e.msg += getopt.show(1);
			break;
		case OE::OPT_UNKNOWN:
		case OE::OPT_NOOPT:
		case OE::OPT_DASH:
		case OE::OPT_LDASH:
		case OE::OPT_DONE:
			e.msg += "\n\n";
			e.msg += getopt.show(0);
			e.msg += "\n\n";
			e.msg += getopt.usage();
			break;
		}
		throw;
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
	for (auto const & acstate : g.acstates) {
		std::cerr << "\t%-22s [%d MHz, %d MHz]\n"_fmt
		             ((""_s + acstate.name + ':').c_str(),
		              acstate.freq_min, acstate.freq_max);
	}
	std::cerr << "CPU Cores\n"
	             "\tCPU cores:             %d\n"
	             "Core Groups\n"_fmt(g.ncpu);
	assert(g.groups && g.ngroups);
	auto * group = &g.groups[0];
	for (coreid_t b = 0, e = 1; e <= g.ncpu; ++e) {
		if (e == g.ncpu || group != g.cores[e].group) {
			std::cerr << "\t%3d:                   [%d, %d]\n"_fmt(b, b, e - 1);
			b = e;
			group = g.cores[e].group;
		}
	}
	std::cerr << "Core Group Frequency Limits\n";
	for (coreid_t i = 0; i < g.ngroups; ++i) {
		std::cerr << "\t%3d:                   [%d MHz, %d MHz]\n"_fmt
		             (i, g.groups[i].min, g.groups[i].max);
	}
	std::cerr << "Load Targets\n";
	for (auto const & acstate : g.acstates) {
		std::cerr << "\t%-22s"_fmt
		             ((""_s + acstate.name + " power target:").c_str())
		          << (acstate.target_load
		              ? " %2d %% load\n"_fmt((acstate.target_load * 100 + 512) / 1024)
		              : " %4d MHz\n"_fmt(acstate.target_freq));
	}
	std::cerr << "Temperature Throttling\n";
	if (g.temp_throttling) {
		std::cerr << "\tactive:                yes\n";
		for (coreid_t i = 0; i < g.ngroups; ++i) {
			auto const & group = g.groups[i];
			std::cerr << "\t%3d:                   [%d C, %d C]\n"_fmt
			             (i, celsius(group.temp_high),
			              celsius(group.temp_crit));
		}
	} else {
		std::cerr << "\tactive:                no\n";
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
	FreqGuard() : freqs{new mhz_t[g.ngroups]} {
		assert(g.groups);
		for (coreid_t groupi = 0; groupi < g.ngroups; ++groupi) {
			auto & group = g.groups[groupi];
			try {
				/* remember clock frequency */
				this->freqs[groupi] = group.freq;
				/* attempt clock frequency write */
				group.freq = this->freqs[groupi];
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
		for (coreid_t groupi = 0; groupi < g.ngroups; ++groupi) {
			auto & group = g.groups[groupi];
			try {
				group.freq = this->freqs[groupi];
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
void signal_recv(int signal) {
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

	/* setup signal handlers */
	sys::sig::Signal sigint{SIGINT, signal_recv};
	sys::sig::Signal sigterm{SIGTERM, signal_recv};
	sys::sig::Signal sighup{SIGHUP, (g.foreground ? signal_recv : SIG_IGN)};

	/* write pid */
	try {
		pidfile.write();
	} catch (sys::sc_error<sys::pid::error> e) {
		fail(Exit::EPID, e,
		     "cannot write to pidfile: "_s + g.pidfilename);
	}

	/* the main loop */
	timing::Cycle sleep;
	while (!g.signal && sleep(g.interval)) {
		update_freq();
	}

	verbose("signal %d received, exiting ..."_fmt(g.signal));
} catch (pid_t otherpid) {
	fail(Exit::ECONFLICT, EEXIST,
	     "a power daemon is already running under PID: %d"_fmt(otherpid));
} catch (sys::sc_error<sys::pid::error> e) {
	fail(Exit::EPID, e,
	     "cannot create pidfile: "_s + g.pidfilename);
} catch (sys::sc_error<sys::sig::error> e) {
	fail(Exit::ESIGNAL, e,
	     "failed to register signal handler: "_s + e.c_str());
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
	} catch (sys::sc_error<sys::sig::error> e) {
		std::cerr << "powerd++: untreated signal setup failure: " << e.c_str() << '\n';
		throw;
	} catch (...) {
		std::cerr << "powerd++: untreated failure\n";
		throw;
	}
	return 0;
}

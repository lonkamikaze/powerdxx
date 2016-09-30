/** \file
 * Implements a library intended to be injected into a clock frequency
 * deamon via LD_PRELOAD.
 *
 * This library reads instructions from std::cin and outputs statistics
 * about the hijacked process on std::cout.
 */

#include <iostream>  /* std::cin, std::cout, std::cerr */
#include <iomanip>   /* std::fixed, std::setprecision */
#include <unordered_map>
#include <map>
#include <string>
#include <regex>
#include <sstream>   /* std::ostringstream, std::istringstream */
#include <memory>    /* std::unique_ptr */
#include <thread>
#include <exception>
#include <mutex>
#include <chrono>    /* std::chrono::steady_clock::now() */
#include <vector>
#include <atomic>

#include <cstring>   /* strncmp() */
#include <cassert>   /* assert() */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/resource.h>  /* CPUSTATES */
#include <libutil.h>       /* struct pidfh */

#include <dlfcn.h>         /* dlfung() */
#include <unistd.h>        /* getpid() */
#include <signal.h>        /* kill() */

#include "utility.hpp"
#include "constants.hpp"
#include "fixme.hpp"

/**
 * File local scope.
 */
namespace {

using constants::CP_TIMES;
using constants::ACLINE;
using constants::FREQ;
using constants::FREQ_LEVELS;

using utility::sprintf;
using namespace utility::literals;

using types::ms;

using fixme::to_string;

/**
 * Safe wrapper around strncmp, which automatically determines the
 * buffer size of s2.
 *
 * @tparam Size
 *	The size of the buffer s2
 * @param s1,s2
 *	The strings to compare
 * @retval 0
 *	Strings are equal
 * @retval !0
 *	Strings are not equal
 */
template <size_t Size>
inline int strcmp(char const * const s1, char const (&s2)[Size]) {
	return strncmp(s1, s2, Size);
}

/**
 * User defined literal for regular expressions.
 *
 * @param str,len
 *	The literal string and its length
 * @return
 *	A regular expression
 */
inline std::regex operator "" _r(char const * const str, size_t const len) {
	return {str, len, std::regex::ECMAScript};
}

/**
 * Represents MIB, but wraps it to provide the necessary operators
 * to use it as an std::map key.
 */
struct mib_t {
	/**
	 * The mib values.
	 */
	int mibs[CTL_MAXNAME];

	/**
	 * Construct a mib with the given number of arguments.
	 *
	 * @tparam Ints
	 *	A list of integer types
	 * @param ints
	 *	A list of integers to create a mib from
	 */
	template <typename... Ints>
	constexpr mib_t(Ints const... ints) : mibs{ints...} {}

	/**
	 * Equality operator required by std::map.
	 *
	 * @param op
	 *	Another mib_t instance
	 * @return
	 *	Whether all values in this and the given mib are
	 *	equal
	 */
	bool operator ==(mib_t const & op) const {
		for (size_t i = 0; i < CTL_MAXNAME; ++i) {
			if (this->mibs[i] != op[i]) {
				return false;
			}
		}
		return true;
	}

	/**
	 * Less than operator required by std::map.
	 *
	 * @param op
	 *	Another mib_t instance
	 * @return
	 *	Whether this mib is less than the given one
	 */
	bool operator < (mib_t const & op) const {
		for (size_t i = 0; i < CTL_MAXNAME; ++i) {
			if (this->mibs[i] != op[i]) {
				return this->mibs[i] < op[i];
			}
		}
		return false;
	}

	/**
	 * Cast to `int *` for value access.
	 *
	 * @return
	 *	A pointer to mibs
	 */
	operator int *() { return mibs; }

	/**
	 * Cast to `int const *` for value access.
	 *
	 * @return
	 *	A pointer to mibs
	 */
	operator int const *() const { return mibs; }
};

/**
 * Implements a recursion safe std::function wrapper.
 *
 * The purpose is to prevent recursive calls of a callback function
 * handle, in cases when a callback function performs actions that
 * cause a successive call of the callback function.
 *
 * To avoid having to return a value when a successive function call
 * occurs only functions returning void are valid callback functions.
 *
 * This is not thread safe.
 *
 * @tparam FunctionArgs
 *	The argument types of the callback function
 */
template <typename... FunctionArgs>
class Callback {
	public:
	/**
	 * The callback function type.
	 */
	typedef std::function<void(FunctionArgs...)> function_t;

	private:
	/**
	 * Storage for the callback function.
	 */
	function_t callback;

	/**
	 * Set if this handle is currently in use.
	 */
	bool called{false};

	public:
	/**
	 * Default constructor, creates a non-callable handle.
	 */
	Callback() : callback{nullptr} {}

	/**
	 * Construct from function.
	 *
	 * @param callback
	 *	The callback function
	 */
	Callback(function_t const & callback) : callback{callback} {}

	/**
	 * Construct from temporary function.
	 *
	 * @param callback
	 *	The callback function
	 */
	Callback(function_t && callback) : callback{std::move(callback)} {}

	/**
	 * Forward call to callback functions.
	 *
	 * @param args
	 *	The arguments to the callback function
	 * @throws std::bad_function_call
	 *	In case this handler was default constructed or constructed
	 *	from a nullptr
	 */
	void operator ()(FunctionArgs... args) {
		if (!this->callback) {
			return;
		}
		if (this->called) {
			return;
		}
		this->called = true;
		this->callback(args...);
		this->called = false;
	}
};

/**
 * Instances of this class represents a specific sysctl value.
 *
 * There should only be one instance of this class per MIB.
 *
 * Instances are thread safe.
 */
class SysctlValue {
	private:
	/**
	 * A stackable mutex.
	 *
	 * nice for exposing methods publicly* and still let them
	 * allow accessing each other.
	 */
	std::recursive_mutex mutable mtx;

	/**
	 * Lock guard type, fitting the mutex.
	 */
	typedef std::lock_guard<decltype(mtx)> lock_guard;

	/**
	 * The sysctl type.
	 */
	unsigned int type;

	/**
	 * The value of the sysctl.
	 *
	 * This is stored as a string and converted to the appropriate
	 * type by the set() and get() methods.
	 */
	std::string value;

	/**
	 * Callback function handle.
	 */
	Callback<SysctlValue &> onSet;

	/**
	 * Callback function type.
	 */
	typedef decltype(onSet)::function_t callback_function;

	public:
	/**
	 * Default constructor.
	 */
	SysctlValue() : type{0}, value{""}, onSet{nullptr} {}

	/**
	 * Copy constructor.
	 *
	 * @param copy
	 *	The instance to copy
	 */
	SysctlValue(SysctlValue const & copy) {
		lock_guard const copylock{copy.mtx};
		this->type = copy.type;
		this->value = copy.value;
		this->onSet = copy.onSet;
	}

	/**
	 * Move constructor.
	 *
	 * @param move
	 *	The instance to move
	 */
	SysctlValue(SysctlValue && move) :
	    type{move.type}, value{std::move(move.value)},
	    onSet{std::move(move.onSet)} {}

	/**
	 * Construct from a type, value and optionally callback tuple.
	 *
	 * @param type
	 *	The CTLTYPE
	 * @param value
	 *	A string representation of the value
	 * @param callback
	 *	A callback function that is called for each set()
	 *	call
	 */
	SysctlValue(unsigned int type, std::string const & value,
	            callback_function const callback = nullptr) :
	    type{type}, value{value}, onSet{callback} {}

	/**
	 * Copy assignment operator.
	 *
	 * @param copy
	 *	The instance to copy
	 * @return
	 *	A self reference
	 */
	SysctlValue & operator =(SysctlValue const & copy) {
		lock_guard const copylock{copy.mtx};
		lock_guard const lock{this->mtx};
		this->type = copy.type;
		this->value = copy.value;
		this->onSet = copy.onSet;
		return *this;
	}

	/**
	 * Move assignment operator.
	 *
	 * @param move
	 *	The instance to move
	 * @return
	 *	A self reference
	 */
	SysctlValue & operator =(SysctlValue && move) {
		lock_guard const lock{this->mtx};
		this->type = move.type;
		this->value = std::move(move.value);
		this->onSet = std::move(move.onSet);
		return *this;
	}

	/**
	 * Returns the required storage size according to the CTLTYPE.
	 *
	 * @return
	 *	The required buffer size to hold the values.
	 * @throws int
	 *	Throws -1 if the current CTLTYPE is not implemented.
	 */
	size_t size() const {
		lock_guard const lock{this->mtx};

		switch (this->type) {
		case CTLTYPE_STRING:
			return this->value.size() + 1;
		case CTLTYPE_INT: {
			std::istringstream stream{this->value};
			size_t count = 1;
			int value;
			while ((stream >> value).good()) { ++count; }
			return count * sizeof(value);
		} case CTLTYPE_LONG: {
			std::istringstream stream{this->value};
			size_t count = 1;
			long value;
			while ((stream >> value).good()) { ++count; }
			return count * sizeof(value);
		} default:
			throw -1;
		}
	}

	/**
	 * Copy a list of values into the given buffer.
	 *
	 * @tparam T
	 *	The type of the values to extract
	 * @param dst,size
	 *	The destination buffer and size
	 * @retval 0
	 *	On success
	 * @retval -1
	 *	On failure to fit all values into the taget buffer,
	 *	also sets errno=ENOMEM
	 */
	template <typename T>
	int get(T * dst, size_t & size) const {
		lock_guard const lock{this->mtx};
		std::istringstream stream{this->value};
		size_t i = 0;
		for (; stream.good(); ++i) {
			if ((i + 1) * sizeof(T) > size) {
				errno = ENOMEM;
				return -1;
			}
			stream >> dst[i];
		}
		size = i * sizeof(T);
		return 0;
	}

	/**
	 * Copy a C string into the given buffer.
	 *
	 * @param dst,size
	 *	The destination buffer and size
	 * @retval 0
	 *	On success
	 * @retval -1
	 *	On failure to fit all values into the taget buffer,
	 *	also sets errno=ENOMEM
	 */
	int get(char * dst, size_t & size) const {
		lock_guard const lock{this->mtx};
		auto const strsize = this->value.size();
		size = std::min(strsize, size - 1);
		for (size_t i = 0; i < size; ++i) { dst[i] = value[i]; }
		dst[size] = 0;
		if (size++ <= strsize) { return 0; }
		errno = ENOMEM;
		return -1;
	}

	/**
	 * Returns a single value.
	 *
	 * @tparam T
	 *	The type of the value
	 * @return
	 *	The value
	 */
	template <typename T>
	T get() const {
		lock_guard const lock{this->mtx};

		std::istringstream stream{this->value};
		T result;
		stream >> result;
		return result;
	}


	/**
	 * Copy a list of values into the given buffer.
	 *
	 * @param dst,size
	 *	The destination buffer and size
	 * @retval 0
	 *	On success
	 * @retval -1
	 *	On failure to fit all values into the taget buffer,
	 *	also sets errno=ENOMEM
	 */
	int get(void * dst, size_t & size) const {
		lock_guard const lock{this->mtx};

		switch (this->type) {
		case CTLTYPE_STRING: {
			return this->get(static_cast<char *>(dst), size);
		} case CTLTYPE_INT:
			return this->get(static_cast<int *>(dst), size);
		case CTLTYPE_LONG:
			return this->get(static_cast<long *>(dst), size);
		default:
			return -1;
		}
	}

	/**
	 * Set this value to the values in the given buffer.
	 *
	 * @tparam T
	 *	The type of the values
	 * @param newp,newlen
	 *	The source buffer and size
	 */
	template <typename T>
	void set(T const * const newp, size_t newlen) {
		std::ostringstream stream;
		for (size_t i = 0; i < newlen / sizeof(T); ++i) {
			stream << (i ? " " : "") << newp[i];
		}
		this->set(stream.str());
	}

	/**
	 * Set this value to the values in the given buffer.
	 *
	 * The buffer will be treated as an array of CTLTYPE values.
	 *
	 * @param newp,newlen
	 *	The source buffer and size
	 */
	int set(void const * const newp, size_t newlen) {
		lock_guard const lock{this->mtx};
		switch (this->type) {
		case CTLTYPE_STRING:
			this->set(std::string{static_cast<char const *>(newp), newlen - 1});
			break;
		case CTLTYPE_INT:
			this->set(static_cast<int const *>(newp), newlen);
			break;
		case CTLTYPE_LONG:
			this->set(static_cast<long const *>(newp), newlen);
			break;
		default:
			return -1;
		}
		return 0;
	}

	/**
	 * Move a string to the value.
	 *
	 * @param value
	 *	The new value
	 */
	void set(std::string && value) {
		lock_guard const lock{this->mtx};
		this->value = std::move(value);
		this->onSet(*this);
	}

	/**
	 * Copy a string to the value.
	 *
	 * @param value
	 *	The new value
	 */
	void set(std::string const & value) {
		lock_guard const lock{this->mtx};
		this->value = value;
		this->onSet(*this);
	}

	/**
	 * Set the value.
	 *
	 * @tparam T
	 *	The value type
	 * @param value
	 *	The value to set
	 */
	template <typename T>
	void set(T const & value) {
		this->set(to_string(value));
	}

	/**
	 * Register a callback function.
	 *
	 * @param callback
	 *	The function to move to the callback handler
	 */
	void registerOnSet(callback_function && callback) {
		lock_guard const lock{this->mtx};
		this->onSet = std::move(callback);
	}

	/**
	 * Register a callback function.
	 *
	 * @param callback
	 *	The function to copy to the callback handler
	 */
	void registerOnSet(callback_function const & callback) {
		lock_guard const lock{this->mtx};
		this->onSet = callback;
	}
};

/**
 * Returns a copy of the value string.
 *
 * @return
 *	The value
 */
template <>
std::string SysctlValue::get<std::string>() const {
	lock_guard const lock{this->mtx};
	return this->value;
}

inline void warn(std::string const & msg) {
	std::cerr << "libloadplay: WARNING: " << msg << std::endl;
}

int sys_results = 0;

inline void fail(std::string const & msg) {
	sys_results = -1;
	std::cerr << "libloadplay: ERROR:   " << msg << std::endl;
}

class {
	private:
	std::mutex mutable mtx;
	typedef std::lock_guard<decltype(mtx)> lock_guard;

	/** name → mib */
	std::unordered_map<std::string, mib_t> mibs{
		{"hw.machine", {CTL_HW, HW_MACHINE}},
		{"hw.model",   {CTL_HW, HW_MODEL}},
		{"hw.ncpu",    {CTL_HW, HW_NCPU}},
		{ACLINE,       {1000}},
		{FREQ,         {1001}},
		{FREQ_LEVELS,  {1002}},
		{CP_TIMES,     {1003}}
	};

	/** mib → (type, value) */
	std::map<mib_t, SysctlValue> sysctls{
		{{CTL_HW, HW_MACHINE}, {CTLTYPE_STRING, "hw.machine"}},
		{{CTL_HW, HW_MODEL},   {CTLTYPE_STRING, "hw.model"}},
		{{CTL_HW, HW_NCPU},    {CTLTYPE_INT,    "0"}},
		{{1000},               {CTLTYPE_INT,    "2"}},
		{{1001},               {CTLTYPE_INT,    "0"}},
		{{1002},               {CTLTYPE_STRING, ""}},
		{{1003},               {CTLTYPE_LONG,   ""}}
	};

	public:
	void addValue(mib_t const & mib, std::string const & value) {
		lock_guard const lock{this->mtx};
		this->sysctls[mib].set(value);
	}

	void addValue(std::string const & name, std::string const & value) {
		lock_guard const lock{this->mtx};

		mib_t mib{};
		try {
			mib = this->mibs.at(name);
		} catch (std::out_of_range &) {
			/* creating a new entry */
			auto const expr = "\\.([0-9]*)\\."_r;
			auto const baseName = std::regex_replace(name, expr, ".%d.");
			/* get the base mib */
			try {
				mib = this->mibs.at(baseName);
			} catch (std::out_of_range &) {
				return warn("unsupported sysctl: "_s + name);
			}
			/* get mib numbers */
			std::smatch match;
			auto str = name;
			for (size_t i = 1;
			     std::regex_search(str, match, expr); ++i) {
				std::istringstream{match[1]} >> mib[i];
				++mib[i]; /* offset, because 0 is the base mib */
				str = match.suffix();
			}
			/* map name → mib */
			this->mibs[name] = mib;
			/* inherit type from base */
			this->sysctls[mib] = this->sysctls[this->mibs[baseName]];
		}
		/* assign value */
		this->sysctls[mib].set(value);
	}

	mib_t const & getMib(std::string const & op) const {
		lock_guard const lock{this->mtx};
		return this->mibs.at(op);
	}

	SysctlValue & operator [](mib_t const & mib) {
		lock_guard const lock{this->mtx};
		return this->sysctls.at(mib);
	}
} sysctls{};

class Emulator {
	private:
	int const ncpu = sysctls[{CTL_HW, HW_NCPU}].get<int>();
	std::unique_ptr<SysctlValue * []> freqs{new SysctlValue *[ncpu]{}};
	std::unique_ptr<int[]> freqRefs{new int[ncpu]{}};
	SysctlValue & cp_times = sysctls[sysctls.getMib(CP_TIMES)];
	std::unique_ptr<unsigned long[]> sum{new unsigned long[CPUSTATES * ncpu]{}};
	std::unique_ptr<unsigned long[]> carry{new unsigned long[ncpu]{}};
	size_t const size = CPUSTATES * ncpu * sizeof(unsigned long);

	public:
	Emulator() {
		/* get freq and freq_levels sysctls */
		std::vector<unsigned long> freqLevels{};
		for (int i = 0; i < this->ncpu; ++i) {
			/* get freqency handler */
			char name[40];
			sprintf(name, FREQ, i);
			try {
				this->freqs[i] = &sysctls[sysctls.getMib(name)];
			} catch (std::out_of_range &) {
				if (i == 0) {
					/* should never be reached */
					fail("missing sysctl: "_s + name);
					throw;
				} else {
					this->freqs[i] = this->freqs[i - 1];
				}
			}

			/* take current clock frequency as reference */
			this->freqRefs[i] = this->freqs[i]->get<int>();

			/* get freq_levels */
			sprintf(name, FREQ_LEVELS, i);
			try {
				auto levels = sysctls[sysctls.getMib(name)]
				              .get<std::string>();
				levels = std::regex_replace(levels, "/[0-9]*"_r, "");
				std::istringstream levelstream{levels};
				freqLevels.clear();
				for (unsigned long level; levelstream.good();) {
					levelstream >> level;
					freqLevels.push_back(level);
				}
			} catch (std::out_of_range &) {
				if (i == 0) {
					/* warning handled in Main::main() */
				}
			}

			this->freqs[i]->registerOnSet([freqLevels](SysctlValue & ctl) {
				auto const freq = ctl.get<int>();
				auto result = freq;
				auto diff = freq + 1000000;
				for (auto lvl : freqLevels) {
					auto lvldiff = (lvl > freq ? lvl - freq : freq - lvl);
					if (lvldiff < diff) {
						diff = lvldiff;
						result = lvl;
					}
				}
				ctl.set(result);
			});
		}

		/* initialise kern.cp_times buffer */
		auto size = this->size;
		try {
			if (size != cp_times.size()) {
				fail("hw.ncpu does not fit the encountered kern.cp_times columns");
			}
		} catch (int) {
			fail("kern.cp_times not initialised");
		}
		cp_times.get(sum.get(), size);

		/* output headers */
		std::cout << "time[s]";
		for (int i = 0; i < this->ncpu; ++i) {
			std::cout << " cpu." << i << ".freq[MHz]"
			          << " cpu." << i << ".recload"
			          << " cpu." << i << ".load";
		}
		std::cout << " max(freqs)[MHz] sum(recloads) max(recloads) sum(loads) max(loads)\n"
		          << std::fixed << std::setprecision(3);
	}

	void operator ()(std::atomic<bool> const * const die) try {
		double statTime = 0.; /* in seconds */
		auto time = std::chrono::steady_clock::now();
		for (uint64_t interval;
		     !*die && (std::cin >> interval).good();) {
			/* reporting variables */
			double statSumRecloads = 0.;
			double statMaxRecloads = 0.;
			double statSumLoads = 0.;
			double statMaxLoads = 0.;
			int statMaxFreq = 0;
			statTime += double(interval) / 1000;
			std::cout << statTime;
			/* perform calculations */
			for (int core = 0; core < this->ncpu; ++core) {
				/* get frame load */
				unsigned long frameSum = 0;
				unsigned long frameLoad = 0;
				for (size_t state = 0; state < CPUSTATES; ++state) {
					unsigned long ticks;
					std::cin >> ticks;
					frameSum += ticks;
					frameLoad += state == CP_IDLE ? 0 : ticks;
				}

				auto const coreFreq = this->freqs[core]->get<int>();
				auto const coreRefFreq = this->freqRefs[core];

				if (!frameSum) {
					std::cout << ' ' << coreFreq << ' ' << 0. << ' ' << 0.;
					continue;
				}

				/* calc recorded stats */
				double const recLoad = double(frameLoad) / frameSum;
				statSumRecloads += recLoad;
				statMaxRecloads = statMaxRecloads > recLoad ? statMaxRecloads : recLoad;
				std::cout << ' ' << coreFreq << ' ' << recLoad;

				/* weigh load and sum */
				frameLoad *= coreRefFreq;
				frameSum *= coreFreq;

				/* add carry load from last frame */
				frameLoad += this->carry[core];
				this->carry[core] = 0;

				/* set load */
				if (frameSum >= frameLoad) {
					sum[core * CPUSTATES + CP_USER] += frameLoad;
					sum[core * CPUSTATES + CP_IDLE] += frameSum - frameLoad;
				} else {
					this->carry[core] = frameLoad - frameSum;
					sum[core * CPUSTATES + CP_USER] += frameSum;
					sum[core * CPUSTATES + CP_IDLE] += 0;
				}

				/* calc stats */
				statMaxFreq = statMaxFreq > coreFreq ? statMaxFreq : coreFreq;
				double const load = double(frameLoad) / frameSum;
				statSumLoads += load;
				statMaxLoads = statMaxLoads > load ? statMaxLoads : load;
				std::cout << ' ' << load;
			}

			/* print stats */
			std::cout << ' ' << statMaxFreq
			          << ' ' << statSumRecloads
			          << ' ' << statMaxRecloads
			          << ' ' << statSumLoads
			          << ' ' << statMaxLoads << '\n';

			/* sleep */
			std::this_thread::sleep_until(time += ms{interval});

			/* commit changes */
			cp_times.set(&sum[0], this->size);
		}

		/* tell process to die */
		kill(getpid(), SIGINT);
	} catch (std::out_of_range &) {
		fail("incomplete emulation setup, please check your load record for complete initialisation");
	}
};

class Main {
	private:
	std::thread bgthread;
	std::atomic<bool> die{false};

	public:
	Main() {
		std::string input;
		std::smatch match;

		/* get static sysctls */
		auto const expr = "([^=]*)=(.*)"_r;
		while (std::getline(std::cin, input) &&
		       std::regex_match(input, match, expr)) {
			sysctls.addValue(match[1].str(), match[2].str());
		}

		/* initialise kern.cp_times */
		try {
			input = input.substr(input.find(' '));
			sysctls.addValue(std::string{CP_TIMES}, input);
		} catch (std::out_of_range &) {
			fail("kern.cp_times cannot be set, please check your load record");
			return;
		}

		/* check for dev.cpu.0.freq */
		char name[40];
		sprintf(name, FREQ, 0);
		try {
			sysctls.getMib(name);
		} catch (std::out_of_range &) {
			fail(""_s + name + " is not set, please check your load record");
			return;
		}

		/* check for dev.cpu.0.freq_levels */
		sprintf(name, FREQ_LEVELS, 0);
		try {
			sysctls.getMib(name);
		} catch (std::out_of_range &) {
			warn(""_s + name + " is not set, please check your load record");
		}


		/* check for hw.ncpu */
		if (sysctls[{CTL_HW, HW_NCPU}].get<int>() < 1) {
			fail("hw.ncpu is not set to a valid value, please check your load record");
			return;
		}

		/* check for hw.acpi.acline */
		try {
			sysctls.getMib(ACLINE);
		} catch (std::out_of_range &) {
			warn(""_s + ACLINE + " is not set, please check your load record");
		}

		/* start background thread */
		try {
			this->bgthread = std::thread{Emulator{}, &this->die};
		} catch (std::out_of_range &) {
			fail("failed to start emulator thread");
			return;
		}
	}

	~Main() {
		this->die = true;
		if (this->bgthread.joinable()) {
			this->bgthread.join();
		}
	}
} main{};

} /* namespace */

/**
 * Functions to intercept.
 */
extern "C" {

typedef int (*fn_sysctl)(const int*, u_int, void*, size_t*, const void*, size_t);
typedef int (*fn_sysctlbyname)(const char*, void*, size_t*, const void*, size_t);
typedef int (*fn_sysctlnametomib)(const char*, int*, size_t*);

static fn_sysctl           orig_sysctl = nullptr;
static fn_sysctlbyname     orig_sysctlbyname = nullptr;
static fn_sysctlnametomib  orig_sysctlnametomib = nullptr;

int sysctl(const int * name, u_int namelen, void * oldp, size_t * oldlenp,
           const void * newp, size_t newlen) try {
	#ifdef EBUG
	fprintf(stderr, "sysctl(%d, %d)\n", name[0], name[1]);
	#endif /* EBUG */
	if (!orig_sysctl) {
		orig_sysctl = (fn_sysctl)dlfunc(RTLD_NEXT, "sysctl");
	}
	/* hard-coded fallbacks */
	if (/* -lpthread */         (namelen == 2 && name[0] == CTL_KERN &&
	                             name[1] == KERN_USRSTACK) ||
	    /* sysctlnametomib() */ (namelen == 2 && name[0] == 0 &&
	                             name[1] == 3)) {
		return orig_sysctl(name, namelen, oldp, oldlenp, newp, newlen);
	}
	mib_t mib{};
	for (size_t i = 0; i < namelen; ++i) {
		mib[i] = name[i];
	}

	if (!oldp && oldlenp) {
		try {
			*oldlenp = sysctls[mib].size();
		} catch (int e) {
			return e;
		}
	}

	if (oldp && oldlenp) {
		if (-1 == sysctls[mib].get(oldp, *oldlenp)) {
			return -1;
		}
	}

	if (newp && newlen) {
		if (-1 == sysctls[mib].set(newp, newlen)) {
			return -1;
		}
	}

	return sys_results;
} catch (std::out_of_range &) {
	errno = ENOENT;
	return -1;
}

int sysctlnametomib(const char * name, int * mibp, size_t * sizep) try {
	#ifdef EBUG
	fprintf(stderr, "sysctlnametomib(%s)\n", name);
	#endif /* EBUG */
	if (!orig_sysctlnametomib) {
		orig_sysctlnametomib = (fn_sysctlnametomib)dlfunc(RTLD_NEXT, "sysctlnametomib");
	}
	auto const & mib = sysctls.getMib(name);
	for (size_t i = 0; i < *sizep; ++i) {
		mibp[i] = mib[i];
	}
	return sys_results;
} catch (std::out_of_range &) {
	errno = ENOENT;
	return -1;
}

int sysctlbyname(const char * name, void * oldp, size_t * oldlenp,
                 const void * newp, size_t newlen) try {
	#ifdef EBUG
	fprintf(stderr, "sysctlbyname(%s)\n", name);
	#endif /* EBUG */
	if (!orig_sysctlbyname) {
		orig_sysctlbyname = (fn_sysctlbyname)dlfunc(RTLD_NEXT, "sysctlbyname");
	}
	if (!orig_sysctlnametomib) {
		orig_sysctlnametomib = (fn_sysctlnametomib)dlfunc(RTLD_NEXT, "sysctlnametomib");
	}
	if (!orig_sysctl) {
		orig_sysctl = (fn_sysctl)dlfunc(RTLD_NEXT, "sysctl");
	}
	int mib[CTL_MAXNAME];
	size_t mibs = CTL_MAXNAME;
	/* explicit fallback for sysctls used by OS functions */
	if (/* malloc() */  strcmp(name, "vm.overcommit") == 0 ||
	    /* -lpthread */ strcmp(name, "kern.smp.cpus") == 0) {
		if (-1 == orig_sysctlnametomib(name, mib, &mibs)) {
			return -1;
		}
		return orig_sysctl(mib, mibs, oldp, oldlenp, newp, newlen);
	}

	/* regular */
	if (-1 == sysctlnametomib(name, mib, &mibs)) {
		return -1;
	}
	return sysctl(mib, mibs, oldp, oldlenp, newp, newlen);
} catch (std::out_of_range &) {
	errno = ENOENT;
	return -1;
}

int daemon(int, int) { return sys_results; }

uid_t geteuid(void) { return sys_results; }

pidfh * pidfile_open(const char *, mode_t, pid_t *) {
	return reinterpret_cast<pidfh *>(&pidfile_open);
}

int pidfile_write(pidfh *) { return sys_results; }

int pidfile_close(pidfh *) { return sys_results; }

int pidfile_remove(pidfh *) { return sys_results; }

int pidfile_fileno(pidfh const *) { return sys_results; }

} /* extern "C" */

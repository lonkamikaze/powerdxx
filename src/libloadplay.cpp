/** \file
 * Implements a library intended to be injected into a clock frequency
 * deamon via LD_PRELOAD.
 *
 * This library reads instructions from std::cin and outputs statistics
 * about the hijacked process on std::cout.
 *
 * The following environment variables affect the operation of loadplay:
 *
 * | Variable     | Description             |
 * |--------------|-------------------------|
 * | LOADPLAY_IN  | Alternative input file  |
 * | LOADPLAY_OUT | Alternative output file |
 */

#include "utility.hpp"
#include "constants.hpp"
#include "fixme.hpp"
#include "version.hpp"
#include "sys/env.hpp"

#include <iostream>  /* std::cin, std::cout, std::cerr */
#include <fstream>   /* std::ifstream, std::ofstream */
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
#include <algorithm> /* std::min() */

#include <cstring>   /* strncmp() */
#include <cassert>   /* assert() */
#include <csignal>   /* raise() */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/resource.h>  /* CPUSTATES */
#include <libutil.h>       /* struct pidfh */

#include <dlfcn.h>         /* dlfung() */
#include <unistd.h>        /* getpid() */

/**
 * File local scope.
 */
namespace {

using constants::CP_TIMES;
using constants::ACLINE;
using constants::FREQ;
using constants::FREQ_LEVELS;

using utility::sprintf_safe;
using namespace utility::literals;
using utility::Sum;

using types::ms;
using types::cptime_t;
using types::mhz_t;
using types::coreid_t;

using fixme::to_string;

using version::LOADREC_FEATURES;
using version::flag_t;
using namespace version::literals;

/**
 * The set of supported features.
 *
 * This value is used to ensure correct input data interpretation.
 */
constexpr flag_t const FEATURES{
	1_FREQ_TRACKING
};

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
 * Calls fprintf(stderr, ...) if built with -DEBUG.
 *
 * @tparam ArgTs
 *	The argument types to forward
 * @param args
 *	Arguments are forwarded to fprintf()
 */
template <typename ... ArgTs>
constexpr void dprintf(ArgTs && ... args) {
#ifdef EBUG
	fprintf(stderr, std::forward<ArgTs>(args) ...);
#endif
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
	 * Initialise from a pointer to an int array.
	 *
	 * @param mibs,len
	 *	The array and its length
	 */
	mib_t(int const * const mibs, u_int const len) : mibs{} {
		for (u_int i = 0; i < len && i < CTL_MAXNAME; ++i) {
			this->mibs[i] = mibs[i];
		}
	}

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
	 * nice for exposing methods publicly and still let them
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

	/**
	 * Provide the size of this value represented as a string of Ts.
	 *
	 * @tparam T
	 *	The type this value is supposed to be a array of
	 * @return
	 *	The size of the whole string of Ts
	 */
	template <typename T>
	size_t size() const {
		lock_guard const lock{this->mtx};
		size_t count = 0;
		T value;
		for (std::istringstream stream{this->value};
		     stream.good(); ++count) {
			stream >> value;
		}
		return count * sizeof(T);
	}

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
		case CTLTYPE_INT:
			return size<int>();
		case CTLTYPE_LONG:
			return size<long>();
		case CTLTYPE_U64:
			return size<uint64_t>();
		default:
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
		case CTLTYPE_STRING:
			return this->get(static_cast<char *>(dst), size);
		case CTLTYPE_INT:
			return this->get(static_cast<int *>(dst), size);
		case CTLTYPE_LONG:
			return this->get(static_cast<long *>(dst), size);
		case CTLTYPE_U64:
			return this->get(static_cast<uint64_t *>(dst), size);
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
		case CTLTYPE_U64:
			this->set(static_cast<uint64_t const *>(newp), newlen);
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

/**
 * Print a debugging message if built with -DEBUG.
 *
 * @param msg
 *	The warning message
 */
inline void debug(std::string const & msg) {
#ifdef EBUG
	std::cerr << "libloadplay: DEBUG: " << msg << std::endl;
#endif
}

/**
 * Print a warning.
 *
 * @param msg
 *	The warning message
 */
inline void warn(std::string const & msg) {
	std::cerr << "libloadplay: WARNING: " << msg << std::endl;
}

/**
 * The success return value of intercepted functions.
 */
int sys_results = 0;

/**
 * This prints an error message and sets sys_results to make the hijacked
 * process fail.
 *
 * @param msg
 *	The error message
 */
inline void fail(std::string const & msg) {
	sys_results = -1;
	std::cerr << "libloadplay: ERROR:   " << msg << std::endl;
}

/**
 * Singleton class representing the sysctl table for this library.
 */
class Sysctls {
	private:
	/**
	 * A simple mutex.
	 */
	std::mutex mutable mtx;

	/**
	 * The appropriate lock guard type for mtx.
	 */
	typedef std::lock_guard<decltype(mtx)> lock_guard;

	/**
	 * Maps name → mib.
	 */
	std::unordered_map<std::string, mib_t> mibs{
		{"hw.machine",     {CTL_HW, HW_MACHINE}},
		{"hw.model",       {CTL_HW, HW_MODEL}},
		{"hw.ncpu",        {CTL_HW, HW_NCPU}},
		{ACLINE,           {1000}},
		{FREQ,             {1001}},
		{FREQ_LEVELS,      {1002}},
		{CP_TIMES,         {1003}},
		{LOADREC_FEATURES, {1004}}
	};

	/**
	 * Maps mib → (type, value).
	 */
	std::map<mib_t, SysctlValue> sysctls{
		{{CTL_HW, HW_MACHINE}, {CTLTYPE_STRING, "hw.machine"}},
		{{CTL_HW, HW_MODEL},   {CTLTYPE_STRING, "hw.model"}},
		{{CTL_HW, HW_NCPU},    {CTLTYPE_INT,    "0"}},
		{{1000},               {CTLTYPE_INT,    "2"}},
		{{1001},               {CTLTYPE_INT,    "0"}},
		{{1002},               {CTLTYPE_STRING, ""}},
		{{1003},               {CTLTYPE_LONG,   ""}},
		{{1004},               {CTLTYPE_U64,    "0"}}
	};

	public:
	/**
	 * Add a value to the sysctls map.
	 *
	 * @param mib
	 *	The mib to add the value for
	 * @param value
	 *	The value to store
	 */
	void addValue(mib_t const & mib, std::string const & value) {
		lock_guard const lock{this->mtx};
		this->sysctls[mib].set(value);
	}

	/**
	 * Add a value to the sysctls map.
	 *
	 * @param name
	 *	The symbolic name of the mib to add the value for
	 * @param value
	 *	The value to store
	 */
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

	/**
	 * Returns a mib for a given symbolic name.
	 *
	 * @param name
	 *	The MIB name
	 * @return
	 *	The MIB
	 */
	mib_t const & getMib(char const * const name) const {
		lock_guard const lock{this->mtx};
		return this->mibs.at(name);
	}

	/**
	 * Returns a reference to a sysctl value container.
	 *
	 * @param name
	 *	The MIB name to return the reference for
	 * @return
	 *	A SysctlValue reference
	 */
	SysctlValue & operator [](char const * const name) {
		return (*this)[getMib(name)];
	}

	/**
	 * Returns a reference to a sysctl value container.
	 *
	 * @param mib
	 *	The MIB to return the reference for
	 * @return
	 *	A SysctlValue reference
	 */
	SysctlValue & operator [](mib_t const & mib) {
		lock_guard const lock{this->mtx};
		return this->sysctls.at(mib);
	}
} sysctls{}; /**< Sole instance of \ref Sysctls. */

/**
 * The reported state of a single CPU pipeline.
 */
struct CoreReport {
	/**
	 * The core clock frequency in [MHz].
	 */
	mhz_t freq;

	/**
	 * The core load as a fraction.
	 */
	double load;
};

/**
 * Print a core clock frequency and load.
 *
 * The clock frequency is printed at 1 MHz resolution, the load at
 * 0.1 MHz.
 *
 * @param out
 *	The stream to print to
 * @param core
 *	The core information to print
 * @return
 *	A reference to the out stream
 */
std::ostream & operator <<(std::ostream & out, CoreReport const & core) {
	return out << " %d %.1f"_fmt(core.freq, core.load * core.freq);
}

/**
 * The report frame information for a single CPU pipeline.
 */
struct CoreFrameReport {
	/**
	 * The recorded core state.
	 */
	CoreReport rec;

	/**
	 * The running core state.
	 */
	CoreReport run;
};

/**
 * Print recorded and running clock frequency and load for a frame.
 *
 * @param out
 *	The stream to print to
 * @param frame
 *	The frame information to print
 * @return
 *	A reference to the out stream
 */
std::ostream & operator <<(std::ostream & out, CoreFrameReport const & frame) {
	return out << frame.rec << frame.run;
}

/**
 * Provides a mechanism to provide frame wise per core load information.
 */
class Report {
	private:
	/**
	 * The output stream to report to.
	 */
	std::ostream & out;

	/**
	 * The number of cpu cores to provide reports for.
	 */
	coreid_t const ncpu;

	/**
	 * The time passed in [ms].
	 */
	Sum<uint64_t> time;

	/**
	 * Per frame per core data.
	 */
	std::unique_ptr<CoreFrameReport[]> cores;

	public:
	/**
	 * Construct a report.
	 *
	 * @param out
	 *	The stream to output to
	 * @param ncpu
	 *	The number of CPU cores to report
	 */
	Report(std::ostream & out, coreid_t const ncpu) :
	    out{out}, ncpu{ncpu}, time{}, cores{new CoreFrameReport[ncpu]{}} {
		out << "time[s]";
		for (coreid_t i = 0; i < ncpu; ++i) {
			out << " cpu.%d.rec.freq[MHz] cpu.%d.rec.load[MHz] cpu.%d.run.freq[MHz] cpu.%d.run.load[MHz]"_fmt
			       (i, i, i, i);
		}
		out << std::endl;
	}

	/**
	 * Represents a frame of the report.
	 *
	 * It provides access to each CoreFrameReport via the subscript
	 * operator [].
	 *
	 * The frame data is output when the frame goes out of scope.
	 */
	class Frame {
		private:
		/**
		 * The report this frame belongs to.
		 */
		Report & report;

		public:
		/**
		 * Construct a report frame.
		 *
		 * @param report
		 *	The report this frame belongs to
		 * @param duration
		 *	The frame duration
		 */
		Frame(Report & report, uint64_t const duration) :
		    report{report} {
			report.time += duration;
		}

		/**
		 * Subscript operator for per core frame report data.
		 *
		 * @param i
		 *	The core index
		 * @return
		 *	A reference to the core frame data
		 */
		CoreFrameReport & operator [](coreid_t const i) {
			assert(i < this->report.ncpu && "out of bounds access");
			return this->report.cores[i];
		}

		/**
		 * Subscript operator for per core frame report data.
		 *
		 * @param i
		 *	The core index
		 * @return
		 *	A const reference to the core frame data
		 */
		CoreFrameReport const & operator [](coreid_t const i) const {
			assert(i < this->report.ncpu && "out of bounds access");
			return this->report.cores[i];
		}

		/**
		 * Finalises the frame by outputting it.
		 */
		~Frame() {
			auto & out = this->report.out;
			out << "%d.%03d"_fmt(this->report.time / 1000,
			                     this->report.time % 1000);
			for (coreid_t i = 0; i < this->report.ncpu; ++i) {
				out << (*this)[i];
			}
			out << std::endl;
		}
	};

	/**
	 * Constructs a frame for this report.
	 *
	 * @tparam ArgTs
	 *	The constructor argument types
	 * @param args
	 *	The constructor arguments
	 */
	template <typename ... ArgTs>
	Frame frame(ArgTs && ... args) {
		return {*this, std::forward<ArgTs>(args) ...};
	}
};

/**
 * Instances of this class represent an emulator session.
 *
 * This should be run in its own thread and expects the sysctl table
 * to be complete.
 */
class Emulator {
	private:
	/**
	 * The input data source.
	 */
	std::istream & cin;

	/**
	 * The output data sink.
	 */
	std::ostream & cout;

	/**
	 * A reference to a bool that tells the emulator to die.
	 */
	bool const & die;

	/**
	 * The size of the kern.cp_times buffer.
	 */
	size_t const size = sysctls[CP_TIMES].size();

	/**
	 * The number of CPUs in kern.cp_times, may be greater than
	 * the hw.ncpu value (e.g. if hyperthreading was turned off).
	 */
	int const ncpu = this->size / sizeof(cptime_t[CPUSTATES]);

	/**
	 * Per core information.
	 */
	struct Core {
		/**
		 * The sysctl handler.
		 *
		 * The constructor ensures this points to a valid handler.
		 */
		SysctlValue * freqCtl{nullptr};

		/**
		 * The clock frequency the simulation is running at.
		 *
		 * Updated at the end of frame and used in the next
		 * frame.
		 */
		mhz_t runFreq{0};

		/**
		 * The recorded clock frequency.
		 *
		 * If FREQ_TRACKING is enabled this is updated at
		 * during the preliminary stage and used at the beginning
		 * of frame stage.
		 */
		mhz_t recFreq{0};

		/**
		 * The load cycles simulated for this frame in [kcycles].
		 *
		 * This is determined at the beginning of frame and used
		 * to calculate the reported load at the end of frame.
		 */
		cptime_t runLoadCycles{0};

		/**
		 * The load cycles carried over to the next frame in [kcycles].
		 *
		 * This is determined at the beginning of frame and
		 * used to calculated the simulation load at the
		 * beginning of the next frame.
		 */
		cptime_t carryLoadCycles{0};
	};

	/**
	 * Simulation state information for each core.
	 */
	std::unique_ptr<Core[]> cores{new Core[this->ncpu]{}};

	/**
	 * The kern.cp_times sysctl handler.
	 */
	SysctlValue & cp_times = sysctls[CP_TIMES];

	/**
	 * The current kern.cp_times values.
	 */
	std::unique_ptr<cptime_t[]> sum{new cptime_t[CPUSTATES * ncpu]{}};

	public:
	/**
	 * The constructor initialises all the members necessary for
	 * emulation.
	 *
	 * It also prints the column headers on stdout.
	 *
	 * @throws std::out_of_range
	 *	In case one of the required sysctls is missing
	 * @param cin,cout
	 *	The character input and output streams
	 * @param die
	 *	If the referenced bool is true, emulation is terminated
	 *	prematurely
	 */
	Emulator(std::istream & cin, std::ostream & cout, bool const & die) :
	    cin{cin}, cout{cout}, die{die} {
		/* get freq and freq_levels sysctls */
		std::vector<mhz_t> freqLevels{};
		for (coreid_t i = 0; i < this->ncpu; ++i) {
			auto & core = this->cores[i];

			/* get freqency handler */
			char name[40];
			sprintf_safe(name, FREQ, i);
			try {
				core.freqCtl = &sysctls[name];
			} catch (std::out_of_range &) {
				if (i == 0) {
					/* should never be reached */
					fail("missing sysctl: "_s + name);
					throw;
				}
				/* fall back to data from the previous core */
				core = this->cores[i - 1];
				continue;
			}

			/* initialise current and reference clock */
			core.runFreq = core.freqCtl->get<mhz_t>();
			core.recFreq = core.runFreq;

			/* get freq_levels */
			sprintf_safe(name, FREQ_LEVELS, i);
			try {
				auto levels = sysctls[name]
				              .get<std::string>();
				levels = std::regex_replace(levels, "/[-+]?[0-9]*"_r, "");
				std::istringstream levelstream{levels};
				freqLevels.clear();

				auto msg = "emulate core %d clock frequencies:"_fmt(i);
				for (unsigned long level; levelstream.good();) {
					levelstream >> level;
					freqLevels.push_back(level);
					msg += " %d"_fmt(level);
				}
				debug(msg);
			} catch (std::out_of_range &) {
				if (i == 0) {
					/* warning handled in Main::main() */
				}
			}

			core.freqCtl->registerOnSet([freqLevels](SysctlValue & ctl) {
				auto const freq = ctl.get<mhz_t>();
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
		cp_times.get(this->sum.get(), size);
	}

	/**
	 * Performs load emulation and prints statistics on cout.
	 *
	 * Reads cin to pull in load changes and updates the
	 * kern.cp_times sysctl to represent the current state.
	 *
	 * When it runs out of load changes it terminates emulation
	 * and sends a SIGINT to the process.
	 */
	void operator ()() try {
		auto const features = sysctls[LOADREC_FEATURES].get<flag_t>();
		Report report(this->cout, this->ncpu);

		auto time = std::chrono::steady_clock::now();
		for (uint64_t duration;
		     !this->die && (this->cin >> duration).good();) {
			/* setup new output frame */
			auto frame = report.frame(duration);

			/*
			 * preliminary
			 */

			/* get recorded core clocks */
			for (coreid_t i = 0; i < this->ncpu; ++i) {
				auto & core = this->cores[i];

				/* update recorded clock frequency */
				if (features & 1_FREQ_TRACKING) {
					this->cin >> core.recFreq;
				}
			}

			/*
			 * beginning of frame
			 */

			/* calculate recorded load */
			for (coreid_t i = 0; i < this->ncpu; ++i) {
				auto & core = this->cores[i];

				/* get recorded ticks */
				double recTicks = 0;
				double recBusyTicks = 0;
				for (size_t state = 0; state < CPUSTATES; ++state) {
					cptime_t ticks;
					this->cin >> ticks;
					recTicks += ticks;
					recBusyTicks += state == CP_IDLE ? 0 : ticks;
				}

				/* get recorded load in [MHz] */
				double const recLoad =
				    recTicks
				    ? recBusyTicks * core.recFreq / recTicks
				    : 0;

				/* update report with recorded data */
				frame[i].rec =
				    {core.recFreq, recLoad / core.recFreq};

				/* get recorded load in [kcycles] */
				cptime_t recLoadCycles = recLoad * duration;

				/* add carry from last frame */
				recLoadCycles += core.carryLoadCycles;

				/* get the load carrying into the next frame,
				 * assume the whole frame runs at the current
				 * clock speed */
				cptime_t const runCycles =
				    core.runFreq * duration;
				core.runLoadCycles =
				    std::min<cptime_t>(runCycles,
				                       recLoadCycles);
				core.carryLoadCycles =
				    recLoadCycles - core.runLoadCycles;

				/* set load for this core */
				this->sum[i * CPUSTATES + CP_USER] +=
				    core.runLoadCycles;
				this->sum[i * CPUSTATES + CP_IDLE] +=
				    runCycles - core.runLoadCycles;
			}

			/* commit changes */
			cp_times.set(&sum[0], this->size);

			/* sleep */
			std::this_thread::sleep_until(time += ms{duration});

			/*
			 * end of frame
			 */

			/* update output */
			for (coreid_t i = 0; i < this->ncpu; ++i) {
				auto & core = this->cores[i];
				core.runFreq = core.freqCtl->get<mhz_t>();
				cptime_t const runCycles =
				    core.runFreq * duration;
				frame[i].run =
				    {core.runFreq,
				     runCycles
				     ? static_cast<double>(core.runLoadCycles) /
				       runCycles
				     : 0};
			}
		}

		/* tell process to die */
		raise(SIGINT);
	} catch (std::out_of_range &) {
		fail("incomplete emulation setup, please check your load record for complete initialisation");
	}
};

/**
 * Singleton class representing the main execution environment.
 */
class Main {
	private:
	/**
	 * The background emulation thread.
	 */
	std::thread bgthread;

	/**
	 * The optional input file.
	 */
	std::ifstream in{sys::env::vars["LOADPLAY_IN"]};

	/**
	 * The optional output file.
	 */
	std::ofstream out{sys::env::vars["LOADPLAY_OUT"]};

	/**
	 * Used to request premature death from the emulation thread.
	 */
	bool die{false};

	public:
	/**
	 * The constructor starts up the emulation.
	 *
	 * - Read the headers from input and populate sysctls
	 * - Ensure the existence of all required sysctls
	 * - Spawn an Emulator instance in its own thread
	 */
	Main() {
		/* check input character stream */
		auto const & env = sys::env::vars;
		if (env["LOADPLAY_IN"] && !this->in) {
			fail("failed to open input file "_s +
			     env["LOADPLAY_IN"].str());
			return;
		}
		std::istream & cin{this->in ? this->in : std::cin};

		std::string input;
		std::smatch match;

		/* get static sysctls */
		auto const expr = "([^=]*)=(.*)"_r;
		while (std::getline(cin, input) &&
		       std::regex_match(input, match, expr)) {
			sysctls.addValue(match[1].str(), match[2].str());
			debug("sysctl %s = %s"_fmt
			      (match[1].str().c_str(), match[2].str().c_str()));
		}

		/* check supported feature flags */
		auto const features = sysctls[LOADREC_FEATURES].get<flag_t>();
		auto const unknown = features & ~FEATURES;
		if (unknown) {
			fail("%s contains unsupported feature flags: %#lx"_fmt
			     (LOADREC_FEATURES, unknown));
			return;
		}

		/* check for dev.cpu.0.freq */
		char name[40];
		sprintf_safe(name, FREQ, 0);
		try {
			sysctls.getMib(name);
		} catch (std::out_of_range &) {
			fail(""_s + name + " is not set, please check your load record");
			return;
		}

		/* check for dev.cpu.0.freq_levels */
		sprintf_safe(name, FREQ_LEVELS, 0);
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

		/* skip frame time */
		input = input.substr(input.find(' ') + 1);

		/* determine the number of cores */
		size_t columns = 1;
		for (auto ch : input) { columns += ch == ' '; }
		/* for each core there is a set of CPUSTATES and a freq column */
		coreid_t const cores = columns / (CPUSTATES + 1);

		/* check reference frequencies */
		std::istringstream istream{input};
		for (coreid_t i = 0;
		     features & 1_FREQ_TRACKING && i < cores; ++i) {
			mhz_t freq;
			istream >> freq;
			input = input.substr(input.find(' ') + 1);
			if (freq <= 0) {
				fail("recorded clock frequencies must be > 0");
				return;
			}
		}

		/* initialise kern.cp_times */
		try {
			sysctls.addValue(std::string{CP_TIMES}, input);
			debug("sysctl %s = %s"_fmt(CP_TIMES, input.c_str()));
		} catch (std::out_of_range &) {
			fail("kern.cp_times cannot be set, please check your load record");
			return;
		}

		/* check output character stream */
		if (env["LOADPLAY_OUT"] && !this->out) {
			fail("failed to open output file "_s +
			     env["LOADPLAY_OUT"].str());
			return;
		}
		std::ostream & cout{this->out ? this->out : std::cout};

		/* start background thread */
		try {
			this->bgthread =
			    std::thread{Emulator{cin, cout, this->die}};
		} catch (std::out_of_range &) {
			fail("failed to start emulator thread");
			return;
		}
	}

	/**
	 * Clean up the background emulation thread.
	 */
	~Main() {
		this->die = true;
		if (this->bgthread.joinable()) {
			this->bgthread.join();
		}
	}
} main{}; /**< Sole instance of \ref Main. */

/**
 * Sets a referenced variable to a given value and restores it when
 * going out of context.
 *
 * @tparam T
 *	The type of the value to hold
 */
template <typename T>
class Hold {
	private:
	T const restore; /**< The original value. */
	T & ref;         /**< Reference to the variable. */
	public:

	/**
	 * The constructor sets the referenced varibale to the given
	 * value.
	 *
	 * @param ref
	 *	The variable to hold and restore
	 * @param value
	 *	The value to set the variable to
	 */
	Hold(T & ref, T const value) : restore{ref}, ref{ref} {
		ref = value;
	}

	/**
	 * Restores the original value.
	 */
	~Hold() {
		this->ref = this->restore;
	}
};

/**
 * Set to activate fallback to the original sysctl functions.
 */
bool sysctl_fallback = false;

} /* namespace */

/**
 * Functions to intercept.
 */
extern "C" {

/**
 * Intercept calls to sysctl().
 *
 * Uses the local \ref anonymous_namespace{libloadplay.cpp}::sysctls
 * store.
 *
 * Falls back to the original under the following conditions:
 *
 * - sysctl_fallback is set
 * - kern.usrstack is requested
 * - vm.* is requested
 *
 * The call may fail for 3 reasons:
 *
 * 1. The fail() function was called and sys_results was assigned -1
 * 2. A target buffer was too small (errno == ENOMEM)
 * 3. The given sysctl is not in the sysctls store (errno == ENOENT)
 *
 * @param name,namelen,oldp,oldlenp,newp,newlen
 *	Please refer to sysctl(3)
 * @retval 0
 *	The call succeeded
 * @retval -1
 *	The call failed
 */
int sysctl(const int * name, u_int namelen, void * oldp, size_t * oldlenp,
           const void * newp, size_t newlen) try {
	static auto const orig =
	    (decltype(&sysctl))dlfunc(RTLD_NEXT, "sysctl");
	if (sysctl_fallback ||
	    /* hard-coded fallback for kern.usrstack, required by -lpthread */
	    (namelen == 2 && name[0] == CTL_KERN && name[1] == KERN_USRSTACK) ||
	    /* hard-coded fallback, stay away from the memory subsystem */
	    (namelen >= 1 && name[0] == CTL_VM)) {
		return orig(name, namelen, oldp, oldlenp, newp, newlen);
	}
	/* must not print the special/fallback cases, because writing
	 * to a stream in one of them causes a SEGFAULT (even when
	 * using sprintf() and fwrite() with a stack buffer) */
	dprintf("sysctl(%d, %d)\n", name[0], name[1]);

	mib_t mib{name, namelen};

	if (oldlenp) {
		if (oldp) {       /* data requested */
			if (-1 == sysctls[mib].get(oldp, *oldlenp)) {
				return -1;
			}
		} else try {      /* size requested */
			*oldlenp = sysctls[mib].size();
		} catch (int e) { /* ….size() may throw */
			return e;
		}
	}

	if (newp && newlen) {     /* update data */
		if (-1 == sysctls[mib].set(newp, newlen)) {
			return -1;
		}
	}

	return sys_results;
} catch (std::out_of_range &) {
	errno = ENOENT;
	return -1;
}

/**
 * Intercept calls to sysctlnametomib().
 *
 * @param name,mibp,sizep
 *	Please refer to sysctl(3)
 * @retval 0
 *	The call succeeded
 * @retval -1
 *	The call failed
 */
int sysctlnametomib(const char * name, int * mibp, size_t * sizep) try {
	static auto const orig =
	    (decltype(&sysctlnametomib)) dlfunc(RTLD_NEXT, "sysctlnametomib");
	if (sysctl_fallback) {
		return orig(name, mibp, sizep);
	}
	/* must not print the special/fallback cases, because writing
	 * to a stream in one of them causes a SEGFAULT (even when
	 * using sprintf() and fwrite() with a stack buffer) */
	dprintf("sysctlnametomib(%s)\n", name);
	auto const & mib = sysctls.getMib(name);
	for (size_t i = 0; i < *sizep && i < CTL_MAXNAME; ++i) {
		mibp[i] = mib[i];
	}
	return sys_results;
} catch (std::out_of_range &) {
	errno = ENOENT;
	return -1;
}

/**
 * Intercept calls to sysctlbyname().
 *
 * Falls back on the original sysctlbyname() for the following names:
 *
 * - kern.smp.cpus
 *
 * May fail for the same reasons as sysctl().
 *
 * @param name,oldp,oldlenp,newp,newlen
 *	Please refer to sysctl(3)
 * @retval 0
 *	The call succeeded
 * @retval -1
 *	The call failed
 */
int sysctlbyname(const char * name, void * oldp, size_t * oldlenp,
                 const void * newp, size_t newlen) {
	static auto const orig =
	    (decltype(&sysctlbyname)) dlfunc(RTLD_NEXT, "sysctlbyname");
	/* explicit fallback for sysctls used by OS functions */
	if (/* -lpthread */ strcmp(name, "kern.smp.cpus") == 0) {
		Hold<bool> hold{sysctl_fallback, true};
		return orig(name, oldp, oldlenp, newp, newlen);
	}
	/* must not print the special/fallback cases, because writing
	 * to a stream in one of them causes a SEGFAULT (even when
	 * using sprintf() and fwrite() with a stack buffer) */
	dprintf("sysctlbyname(%s)\n", name);
	/* use original function for regular operation, just without
	 * holding the sysctl_fallback flag */
	return orig(name, oldp, oldlenp, newp, newlen);
}

/**
 * Intercept calls to daemon().
 *
 * Prevents process from separating from the controlling terminal.
 *
 * @return
 *	The value of sys_results
 */
int daemon(int, int) { return sys_results; }

/**
 * Intercept calls to geteuid().
 *
 * Tells the asking process that it is running as root.
 *
 * @return
 *	Always returns 0
 */
uid_t geteuid(void) { return 0; }

/**
 * Intercept calls to pidfile_open().
 *
 * Prevents pidfile locking and creation by the hijacked process.
 *
 * @return
 *	A dummy pointer
 */
pidfh * pidfile_open(const char *, mode_t, pid_t *) {
	return reinterpret_cast<pidfh *>(&pidfile_open);
}

/**
 * Intercept calls to pidfile_write().
 *
 * @return
 *	The value of sys_results
 */
int pidfile_write(pidfh *) { return sys_results; }

/**
 * Intercept calls to pidfile_close().
 *
 * @return
 *	The value of sys_results
 */
int pidfile_close(pidfh *) { return sys_results; }

/**
 * Intercept calls to pidfile_remove().
 *
 * @return
 *	The value of sys_results
 */
int pidfile_remove(pidfh *) { return sys_results; }

/**
 * Intercept calls to pidfile_fileno().
 *
 * @return
 *	The value of sys_results
 */
int pidfile_fileno(pidfh const *) { return sys_results; }

} /* extern "C" */
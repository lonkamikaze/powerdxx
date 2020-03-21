/**
 * Implements a library intended to be injected into a clock frequency
 * deamon via LD_PRELOAD.
 *
 * This library reads instructions from io::fin (stdin) and outputs
 * statistics about the hijacked process on io::fout (stdout).
 *
 * The following environment variables affect the operation of loadplay:
 *
 * | Variable     | Description             |
 * |--------------|-------------------------|
 * | LOADPLAY_IN  | Alternative input file  |
 * | LOADPLAY_OUT | Alternative output file |
 *
 * @file
 */

#include "utility.hpp"
#include "constants.hpp"
#include "version.hpp"
#include "sys/env.hpp"
#include "sys/io.hpp"

#include <unordered_map>
#include <map>
#include <string>
#include <regex>
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
using constants::FREQ_DRIVER;
using constants::TEMPERATURE;
using constants::TJMAX_SOURCES;

using utility::sprintf_safe;
using namespace utility::literals;
using utility::Sum;
using utility::FromChars;

using types::ms;
using types::cptime_t;
using types::mhz_t;
using types::coreid_t;

using version::LOADREC_FEATURES;
using version::flag_t;
using namespace version::literals;

namespace io = sys::io;

/**
 * Output file type alias.
 *
 * @tparam Ownership
 *	The io::ownership type of the file
 */
template <auto Ownership> using ofile = io::file<Ownership, io::write>;

/**
 * Input file type alias.
 *
 * @tparam Ownership
 *	The io::ownership type of the file
 */
template <auto Ownership> using ifile = io::file<Ownership, io::read>;

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
 * Calls io::ferr.printf(...) if built with -DEBUG.
 *
 * @tparam ArgTs
 *	The argument types to forward
 * @param args
 *	Arguments are forwarded to fprintf()
 */
template <typename ... ArgTs>
constexpr void dprintf(ArgTs && ... args) {
#ifdef EBUG
	io::ferr.printf(std::forward<ArgTs>(args) ...);
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
		for (auto fetch = FromChars{this->value};
		     fetch(value); ++count);
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
	 *	On failure to fit all values into the target buffer,
	 *	also sets errno=ENOMEM
	 */
	template <typename T>
	int get(T * dst, size_t & size) const {
		lock_guard const lock{this->mtx};
		size /= sizeof(T);
		size_t i = 0;
		auto fetch = FromChars{this->value};
		for (; i < size && fetch(dst[i]); ++i);
		size = i * sizeof(T);
		return errno = fetch * ENOMEM, -fetch;
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
		T result{};
		auto fetch = FromChars{this->value};
		if (!fetch(result)) {
			return errno = EINVAL, result;
		}
		return errno = fetch * ENOMEM, result;
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
		std::string value;
		for (size_t i = 0; i < newlen / sizeof(T); ++i) {
			value += (i ? " " : "") + std::to_string(newp[i]);
		}
		set(std::move(value));
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
			errno=EFAULT;
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
		this->set(std::to_string(value));
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
 * @tparam MsgTs
 *	The message argument types
 * @param msg
 *	The debugging message
 * @return
 *	An output file handle for extending the message
 */
template <typename... MsgTs>
inline ofile<io::link> debug(MsgTs &&... msg) {
#ifdef EBUG
	io::ferr.print("libloadplay: DEBUG: ");
	return io::ferr.printf(std::forward<MsgTs>(msg)...);
#else
	return ofile<io::link>{nullptr};
#endif
}

/**
 * Print a warning.
 *
 * @tparam MsgTs
 *	The message argument types
 * @param msg
 *	The warning message
 * @return
 *	An output file handle for extending the message
 */
template <typename... MsgTs>
inline ofile<io::link> warn(MsgTs &&... msg) {
	io::ferr.print("libloadplay: WARNING: ");
	return io::ferr.printf(std::forward<MsgTs>(msg)...);
}

/**
 * The success return value of intercepted functions.
 */
int sys_results = 0;

/**
 * This prints an error message and sets sys_results to make the hijacked
 * process fail.
 *
 * @tparam MsgTs
 *	The message argument types
 * @param msg
 *	The error message
 * @return
 *	An output file handle for extending the message
 */
template <typename... MsgTs>
inline ofile<io::link> fail(MsgTs &&... msg) {
	sys_results = -1;
	io::ferr.print("libloadplay: ERROR:   ");
	return io::ferr.printf(std::forward<MsgTs>(msg)...);
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
		{FREQ,             {1001, -1}},
		{FREQ_LEVELS,      {1002, -1}},
		{CP_TIMES,         {1003}},
		{LOADREC_FEATURES, {1004}},
		{FREQ_DRIVER,      {1005, -1}},
		{TEMPERATURE,      {1006, -1}},
		{TJMAX_SOURCES[0], {1007, -1}}
	};

	/**
	 * Maps mib → (type, value).
	 */
	std::map<mib_t, SysctlValue> sysctls{
		{{CTL_HW, HW_MACHINE}, {CTLTYPE_STRING, "hw.machine"}},
		{{CTL_HW, HW_MODEL},   {CTLTYPE_STRING, "hw.model"}},
		{{CTL_HW, HW_NCPU},    {CTLTYPE_INT,    "0"}},
		{{1000},               {CTLTYPE_INT,    "2"}},
		{{1001, -1},           {CTLTYPE_INT,    "0"}},
		{{1002, -1},           {CTLTYPE_STRING, ""}},
		{{1003},               {CTLTYPE_LONG,   ""}},
		{{1004},               {CTLTYPE_U64,    "0"}},
		{{1005, -1},           {CTLTYPE_STRING, ""}},
		{{1006, -1},           {CTLTYPE_INT,    "-1"}},
		{{1007, -1},           {CTLTYPE_INT,    "-1"}},
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
		try {
			lock_guard const lock{this->mtx};
			this->sysctls[this->mibs.at(name)].set(value);
		} catch (std::out_of_range &) {
			/* get the base mib */
			mib_t mib{}, baseMib{};
			try {
				mib = baseMib = getBaseMib(name.c_str());
			} catch (std::out_of_range &) {
				warn("unsupported sysctl: %s\n", name.c_str());
				return;
			}
			/* get mib numbers */
			std::smatch match;
			auto const expr = "\\.([0-9]+)\\."_r;
			if (std::regex_search(name, match, expr) &&
			    FromChars{match[1]}(mib[1])) {
				lock_guard const lock{this->mtx};
				/* map name → mib */
				this->mibs[name] = mib;
				/* inherit type from base */
				(this->sysctls[mib] = this->sysctls[baseMib]).set(value);
			}
		}
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
	 * Retrieves the base mib for a given mib name.
	 *
	 * E.g. the base mib for "dev.cpu.0.freq" is the mib for
	 * "dev.cpu.%d.freq".
	 *
	 * @param name
	 *	The MIB name
	 * @return
	 *	The MIB of the base name
	 */
	mib_t const & getBaseMib(char const * const name) const {
		lock_guard const lock{this->mtx};
		auto const expr = "\\.([0-9]+)\\."_r;
		auto const baseName = std::regex_replace(name, expr, ".%d.");
		return this->mibs.at(baseName);
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
 * The clock frequency is printed at 1 MHz resolution, the load at
 * 0.1 MHz.
 *
 * @param fout
 *	The stream to print to
 * @param frame
 *	The frame information to print
 * @return
 *	A reference to the out stream
 */
ofile<io::link>
operator <<(ofile<io::link> fout, CoreFrameReport const & frame) {
	return fout.printf(" %d %.1f %d %.1f",
	                   frame.rec.freq, frame.rec.load * frame.rec.freq,
	                   frame.run.freq, frame.run.load * frame.run.freq);
}

/**
 * Provides a mechanism to provide frame wise per core load information.
 */
class Report {
	private:
	/**
	 * The output stream to report to.
	 */
	ofile<io::link> fout;

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
	 * @param fout
	 *	The stream to output to
	 * @param ncpu
	 *	The number of CPU cores to report
	 */
	Report(ofile<io::link> fout, coreid_t const ncpu) :
	    fout{fout}, ncpu{ncpu}, time{}, cores{new CoreFrameReport[ncpu]{}} {
		fout.print("time[s]");
		for (coreid_t i = 0; i < ncpu; ++i) {
			fout.printf(" cpu.%d.rec.freq[MHz] cpu.%d.rec.load[MHz]"
			            " cpu.%d.run.freq[MHz] cpu.%d.run.load[MHz]",
			            i, i, i, i);
		}
		fout.putc('\n').flush();
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
			auto fout = this->report.fout;
			fout.printf("%d.%03d", this->report.time / 1000,
			            this->report.time % 1000);
			for (coreid_t i = 0; i < this->report.ncpu; ++i) {
				fout << (*this)[i];
			}
			fout.putc('\n').flush();
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
	ifile<io::link> fin;

	/**
	 * The output data sink.
	 */
	ofile<io::link> fout;

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
		 * The cycles carried over to the next frame in [kcycles].
		 *
		 * This is determined at the beginning of frame and
		 * used to calculated the simulation load at the
		 * beginning of the next frame.
		 */
		cptime_t carryCycles[CPUSTATES]{};
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
	 * @param fin,fout
	 *	The character input and output streams
	 * @param die
	 *	If the referenced bool is true, emulation is terminated
	 *	prematurely
	 */
	Emulator(ifile<io::link> fin, ofile<io::link> fout, bool const & die) :
	    fin{fin}, fout{fout}, die{die} {
		/* get freq and freq_levels sysctls */
		std::vector<mhz_t> freqLevels{};
		for (coreid_t i = 0; i < this->ncpu; ++i) {
			auto & core = this->cores[i];

			/* get frequency handler */
			char name[40];
			sprintf_safe(name, FREQ, i);
			try {
				core.freqCtl = &sysctls[name];
			} catch (std::out_of_range &) {
				if (i == 0) {
					/* should never be reached */
					fail("missing sysctl: %s\n", name);
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
				auto fetch = FromChars{levels};
				freqLevels.clear();

				auto msg = debug("emulate core %d clock frequencies:", i);
				for (unsigned long level{0}; fetch(level);) {
					freqLevels.push_back(level);
					msg.printf(" %d", level);
				}
				msg.putc('\n');
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
	 * Performs load emulation and prints statistics on io::fout.
	 *
	 * Reads fin to pull in load changes and updates the
	 * kern.cp_times sysctl to represent the current state.
	 *
	 * When it runs out of load changes it terminates emulation
	 * and sends a SIGINT to the process.
	 */
	void operator ()() try {
		auto const features = sysctls[LOADREC_FEATURES].get<flag_t>();
		Report report(this->fout, this->ncpu);

		auto time = std::chrono::steady_clock::now();
		for (uint64_t duration;
		     !this->die && (1 == this->fin.scanf("%ju", duration));) {
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
					this->fin.scanf("%u", core.recFreq);
				}
			}

			/*
			 * beginning of frame
			 */

			/* calculate recorded load */
			for (coreid_t i = 0; i < this->ncpu; ++i) {
				auto & core = this->cores[i];

				/* get recorded ticks */
				cptime_t recTicks[CPUSTATES]{};
				cptime_t sumRecTicks{0};
				for (auto & ticks : recTicks) {
					this->fin.scanf("%lu", ticks);
					sumRecTicks += ticks;
				}
				/* must be non-zero */
				sumRecTicks += !sumRecTicks;
				/* update report with recorded data */
				frame[i].rec =
				    {core.recFreq, 1. - static_cast<double>(recTicks[CP_IDLE]) / sumRecTicks};

				/* get recorded cycles in [kcycles] */
				cptime_t cycles[CPUSTATES]{};
				auto const runCycles = duration * core.recFreq;
				for (cptime_t state = 0; state < CPUSTATES; ++state) {
					cycles[state] = runCycles * recTicks[state] / sumRecTicks;
				}

				/* add the carry to the recorded cycles */
				for (cptime_t state = 0; state < CPUSTATES; ++state) {
					cycles[state] += core.carryCycles[state];
					core.carryCycles[state] = 0;
				}

				/* determine simulation cycles at current freq */
				cptime_t availableCycles = core.runFreq * duration;
				core.runLoadCycles = 0;
				/* assign cycles in order of priority */
				static_assert(CPUSTATES == 5, "All CPUSTATES must be implemented");
				for (auto state : {CP_INTR, CP_SYS, CP_USER, CP_NICE}) {
					if (availableCycles >= cycles[state]) {
						availableCycles -= cycles[state];
						core.carryCycles[state] = 0;
					} else {
						core.carryCycles[state] =
						    cycles[state] - availableCycles;
						cycles[state] = availableCycles;
						availableCycles = 0;
					}
					core.runLoadCycles += cycles[state];
				}
				/* assign leftovers to the idle state */
				cycles[CP_IDLE] = availableCycles;

				/* set load for this core */
				for (cptime_t state = 0; state < CPUSTATES; ++state) {
					this->sum[i * CPUSTATES + state] +=
					    cycles[state];
				}
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
		if (!this->die) {
			raise(SIGINT);
		}
	} catch (std::out_of_range &) {
		fail("incomplete emulation setup, please check your load record for complete initialisation\n");
	}
};

/**
 * Set to activate fallback to the original sysctl functions.
 *
 * This is reset when Main initialisation completes.
 */
bool sysctl_startup = true;

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
	ifile<io::own> fin;

	/**
	 * The optional output file.
	 */
	ofile<io::own> fout;

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
		ifile<io::link> fin{io::fin};
		if (env["LOADPLAY_IN"] &&
		    !(fin = this->fin = {env["LOADPLAY_IN"], "rb"})) {
			fail("failed to open input file %s\n",
			     env["LOADPLAY_IN"].c_str());
			return;
		}

		char inbuf[16384]{};
		std::cmatch match;

		/* get static sysctls */
		auto const expr = "([^=]*)=(.*)\n"_r;
		if (!fin.gets(inbuf)) {
			fail("cannot read from input\n");
			return;
		}
		while (std::regex_match(inbuf, match, expr)) {
			sysctls.addValue(match[1].str(), match[2].str());
			debug("sysctl %s = %s\n",
			      match[1].str().c_str(), match[2].str().c_str());
			if (!fin.gets(inbuf)) {
				fail("unexpected end of input behind: %s", inbuf);
				return;
			}
		}

		/* check supported feature flags */
		auto const features = sysctls[LOADREC_FEATURES].get<flag_t>();
		auto const unknown = features & ~FEATURES;
		if (unknown) {
			fail("%s contains unsupported feature flags: %#lx\n",
			     LOADREC_FEATURES, unknown);
			return;
		}

		/* check for dev.cpu.0.freq */
		char name[40];
		sprintf_safe(name, FREQ, 0);
		try {
			sysctls.getMib(name);
		} catch (std::out_of_range &) {
			fail("%s is not set, please check your load record\n", name);
			return;
		}

		/* check for dev.cpu.0.freq_levels */
		sprintf_safe(name, FREQ_LEVELS, 0);
		try {
			sysctls.getMib(name);
		} catch (std::out_of_range &) {
			warn("%s is not set, please check your load record\n", name);
		}

		/* check for hw.ncpu */
		if (sysctls[{CTL_HW, HW_NCPU}].get<int>() < 1) {
			fail("hw.ncpu is not set to a valid value, please check your load record\n");
			return;
		}

		/* check for hw.acpi.acline */
		try {
			sysctls.getMib(ACLINE);
		} catch (std::out_of_range &) {
			warn("%s is not set, please check your load record", ACLINE);
		}

		/* skip frame time */
		auto fetch = FromChars{inbuf};
		if (uint64_t time{1}; !fetch(time) || time != 0) {
			fail("first frame time must be 0: %.8s\n", inbuf);
			return;
		}

		/* determine the number of cores */
		size_t columns = 0;
		auto seek = fetch;
		for (cptime_t val{0}; seek(val); ++columns);
		coreid_t const cores = columns / (CPUSTATES + !!(features & 1_FREQ_TRACKING));

		/* check reference frequencies */
		for (coreid_t i = 0;
		     features & 1_FREQ_TRACKING && i < cores; ++i) {
			mhz_t freq{0};
			if (!fetch(freq)) {
				fail("unable to parse core frequency from record at: %.8s ...\n", fetch.it);
				return;
			}
			if (freq <= 0) {
				fail("recorded clock frequencies must be > 0\n");
				return;
			}
		}

		/* initialise kern.cp_times */
		try {
			sysctls.addValue(std::string{CP_TIMES}, fetch.it);
			debug("sysctl %s = %s", CP_TIMES, fetch.it);
		} catch (std::out_of_range &) {
			fail("kern.cp_times cannot be set, please check your load record\n");
			return;
		}

		/* check output character stream */
		ofile<io::link> fout{io::fout};
		if (env["LOADPLAY_OUT"] &&
		    !(fout = this->fout = {sys::env::vars["LOADPLAY_OUT"], "wb"})) {
			fail("failed to open output file %s\n",
			     env["LOADPLAY_OUT"].c_str());
			return;
		}

		/* start background thread */
		try {
			this->bgthread =
			    std::thread{Emulator{fin, fout, this->die}};
			sysctl_startup = false;
		} catch (std::out_of_range &) {
			fail("failed to start emulator thread\n");
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
 * - sysctl_startup is set
 * - The mib is not known to the simulation
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
           const void * newp, size_t newlen) {
	static auto const orig =
	    (decltype(&sysctl))dlfunc(RTLD_NEXT, "sysctl");

	/* do not access the sysctl store during startup */
	if (sysctl_startup) {
		return orig(name, namelen, oldp, oldlenp, newp, newlen);
	}

	/* try simulated sysctls */
	try {
		mib_t mib{name, namelen};
		auto & value = sysctls[mib];
		dprintf("sysctl(%d, %d) [sim]\n", name[0], name[1]);

		if (oldlenp) {
			if (oldp) {
				/* data requested */
				if (-1 == value.get(oldp, *oldlenp)) {
					return -1;
				}
			} else {
				/* size requested */
				*oldlenp = value.size();
			}
		}

		if (newp && newlen) {
			/* update data */
			if (-1 == value.set(newp, newlen)) {
				return -1;
			}
		}

		return sys_results;
	} catch (std::out_of_range &) {}

	/* fallback to system sysctl */
	dprintf("sysctl(%d, %d) [sys]\n", name[0], name[1]);
	return orig(name, namelen, oldp, oldlenp, newp, newlen);
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
int sysctlnametomib(const char * name, int * mibp, size_t * sizep) {
	static auto const orig =
	    (decltype(&sysctlnametomib)) dlfunc(RTLD_NEXT, "sysctlnametomib");
	/* do not access the sysctl store during startup */
	if (sysctl_startup) {
		return orig(name, mibp, sizep);
	}

	/* try simulated sysctls */
	try {
		auto const & mib = sysctls.getMib(name);
		dprintf("sysctlnametomib(%s) [sim]\n", name);
		for (size_t i = 0; i < *sizep && i < CTL_MAXNAME; ++i) {
			mibp[i] = mib[i];
		}
		return sys_results;
	} catch (std::out_of_range &) {}

	/* error if the base is known, that means it is a simulation
	 * variable and since the requested instance does not exist
	 * within the simulation, it must not be accessible to the
	 * host process */
	try {
		sysctls.getBaseMib(name);
		dprintf("sysctlnametomib(%s) [sim] -> ENOENT\n", name);
		return errno=ENOENT, -1;
	} catch (std::out_of_range &) {}

	/* fallback to system sysctl */
	dprintf("sysctlnametomib(%s) [sys]\n", name);
	return orig(name, mibp, sizep);
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

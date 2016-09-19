#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <map>
#include <string>
#include <regex>
#include <sstream>
#include <memory>
#include <thread>
#include <exception>
#include <mutex>
#include <chrono>    /* std::chrono::steady_clock::now() */
#include <vector>

#include <cstring>
#include <cassert>

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/resource.h>  /* CPUSTATES */
#include <libutil.h>       /* struct pidfh */

#include <dlfcn.h>
#include <unistd.h>        /* getpid() */
#include <signal.h>        /* kill() */

#include "utility.hpp"
#include "constants.hpp"
#include "fixme.hpp"

namespace {

using constants::CP_TIMES;
using constants::ACLINE;
using constants::FREQ;
using constants::FREQ_LEVELS;

using utility::sprintf;
using namespace utility::literals;

using types::ms;

using fixme::to_string;

template <size_t Size, typename... Args>
inline int strcmp(char const * const s1, char const (&s2)[Size]) {
	return strncmp(s1, s2, Size);
}

inline std::regex operator "" _r(char const * const str, size_t const len) {
	return {str, len, std::regex::ECMAScript};
}

struct mib_t {
	int mibs[CTL_MAXNAME];

	template <typename... Ints>
	constexpr mib_t(Ints const... ints) : mibs{ints...} {}

	bool operator ==(mib_t const & op) const {
		for (size_t i = 0; i < CTL_MAXNAME; ++i) {
			if (this->mibs[i] != op[i]) {
				return false;
			}
		}
		return true;
	}

	bool operator < (mib_t const & op) const {
		for (size_t i = 0; i < CTL_MAXNAME; ++i) {
			if (this->mibs[i] != op[i]) {
				return this->mibs[i] < op[i];
			}
		}
		return false;
	}

	operator int *() { return mibs; }
	operator int const *() const { return mibs; }
};

class SysctlValue {
	private:
	std::mutex mutable mtx;
	unsigned int type;
	std::string value;

	template <typename T>
	int doGet(T * dst, size_t & size) const {
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

	template <typename T>
	void doSet(T const * const newp, size_t newlen) {
		std::ostringstream stream;
		for (size_t i = 0; i < newlen / sizeof(T); ++i) {
			stream << (i ? " " : "") << newp[i];
		}
		this->value = stream.str();
	}

	public:
	SysctlValue() : type{0}, value{""} {}

	SysctlValue(SysctlValue const & copy) {
		std::lock_guard<std::mutex> const lock{copy.mtx};
		this->type = copy.type;
		this->value = copy.value;
	}

	SysctlValue(SysctlValue && move) :
	    type{move.type}, value{std::move(move.value)} {}

	SysctlValue(unsigned int type, std::string const & value) :
	    type{type}, value{value} {}

	SysctlValue & operator =(SysctlValue const & copy) {
		std::lock_guard<std::mutex> const lock{copy.mtx};
		this->type = copy.type;
		this->value = copy.value;
		return *this;
	}

	SysctlValue & operator =(SysctlValue && move) {
		this->type = move.type;
		this->value = std::move(move.value);
		return *this;
	}

	size_t size() const {
		std::lock_guard<std::mutex> const lock{this->mtx};

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
			throw -1; /* TODO unsupportedType */
		}
	}

	template <typename T>
	T get() {
		std::lock_guard<std::mutex> const lock{this->mtx};

		std::istringstream stream{this->value};
		T result;
		stream >> result;
		return result;
	}

	int get(void * dst, size_t & size) const {
		std::lock_guard<std::mutex> const lock{this->mtx};

		switch (this->type) {
		case CTLTYPE_STRING: {
			size_t i = 0;
			for (; i < this->value.size(); ++i) {
				if (i + 1 >= size) {
					static_cast<char *>(dst)[i] = 0;
					errno = ENOMEM;
					return -1;
				}
				static_cast<char *>(dst)[i] = value[i];
			}
			static_cast<char *>(dst)[i] = 0;
			size = i + 1;
			return 0;
		} case CTLTYPE_INT:
			return this->doGet(static_cast<int *>(dst), size);
		case CTLTYPE_LONG:
			return this->doGet(static_cast<long *>(dst), size);
		default:
			return -1;
		}
	}

	int set(void const * const newp, size_t newlen) {
		std::lock_guard<std::mutex> const lock{this->mtx};

		switch (this->type) {
		case CTLTYPE_STRING:
			this->value = std::string{static_cast<char const *>(newp), newlen - 1};
			break;
		case CTLTYPE_INT:
			this->doSet(static_cast<int const *>(newp), newlen);
			break;
		case CTLTYPE_LONG:
			this->doSet(static_cast<long const *>(newp), newlen);
			break;
		default:
			return -1;
		}
		return 0;
	}

	void set(std::string const & value) {
		std::lock_guard<std::mutex> const lock{this->mtx};
		this->value = value;
	}

	template <typename T>
	void set(T const value) {
		std::lock_guard<std::mutex> const lock{this->mtx};
		this->value = to_string(value);
	}
};

template <>
std::string SysctlValue::get<std::string>() {
	std::lock_guard<std::mutex> const lock{this->mtx};
	return this->value;
}

class {
	private:
	std::mutex mutable mtx;

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
		{{CTL_HW, HW_NCPU},    {CTLTYPE_INT,    "hw.ncpu"}},
		{{1000},               {CTLTYPE_INT,    ACLINE}},
		{{1001},               {CTLTYPE_INT,    FREQ}},
		{{1002},               {CTLTYPE_STRING, FREQ_LEVELS}},
		{{1003},               {CTLTYPE_LONG,   CP_TIMES}}
	};

	public:
	void addValue(mib_t const & mib, std::string const & value) {
		std::lock_guard<std::mutex> const lock{this->mtx};
		this->sysctls[mib].set(value);
	}

	void addValue(std::string const & name, std::string const & value) {
		std::lock_guard<std::mutex> const lock{this->mtx};

		mib_t mib{};
		try {
			mib = this->mibs.at(name);
		} catch (std::out_of_range &) {
			/* creating a new entry */
			auto const expr = "\\.([0-9]*)\\."_r;
			auto const baseName = std::regex_replace(name, expr, ".%d.");
			/* get the base mib */
			mib = this->mibs.at(baseName); /* TODO handle throw */
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
		std::lock_guard<std::mutex> const lock{this->mtx};
		return this->mibs.at(op);
	}

	SysctlValue & operator [](mib_t const & mib) {
		std::lock_guard<std::mutex> const lock{this->mtx};
		return this->sysctls.at(mib);
	}
} sysctls{};

class Emulator {
	private:
	int const ncpu = sysctls[{CTL_HW, HW_NCPU}].get<int>();
	std::unique_ptr<SysctlValue * []> freqs{new SysctlValue *[ncpu]{}};
	std::unique_ptr<int[]> freqRefs{new int[ncpu]{}};
	SysctlValue & cp_times = sysctls[sysctls.getMib("kern.cp_times")];
	std::unique_ptr<unsigned long[]> sum{new unsigned long[CPUSTATES * ncpu]{}};
	std::unique_ptr<unsigned long[]> carry{new unsigned long[ncpu]{}};
	size_t const size = CPUSTATES * ncpu * sizeof(unsigned long);
	std::unique_ptr<std::vector<unsigned long>[]> levels{new std::vector<unsigned long>[ncpu]{}};

	/**
	 * Fix core frequencies to the available frequency levels.
	 */
	void fixFreqs() {
		for (int i = 0; i < this->ncpu; ++i) {
			auto const freq = this->freqs[i]->get<int>();
			auto diff = freq + 1000000;
			for (auto lvl : this->levels[i]) {
				auto lvldiff = (lvl > freq ? lvl - freq : freq - lvl);
				if (lvldiff < diff) {
					diff = lvldiff;
					this->freqs[i]->set(lvl);
				}
			}
		}
	}

	public:
	Emulator() {
		/* get freq and freq_levels sysctls */
		for (int i = 0; i < this->ncpu; ++i) {
			/* get freqency handler */
			char name[40];
			sprintf(name, FREQ, i);
			try {
				this->freqs[i] = &sysctls[sysctls.getMib(name)];
			} catch (std::out_of_range &) {
				if (i == 0) {
					std::cerr << "TODO WTF\n";
					throw;
				}
				this->freqs[i] = this->freqs[i - 1];
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
				for (unsigned long level; levelstream.good();) {
					levelstream >> level;
					this->levels[i].push_back(level);
				}
			} catch (std::out_of_range &) {
				if (i == 0) {
					std::cerr << "TODO WTF\n";
					throw;
				}
				this->levels[i] = this->levels[i - 1];
			}
		}

		/* initialise kern.cp_times buffer */
		auto size = this->size;
		assert(size == cp_times.size());
		cp_times.get(sum.get(), size);

		/* output headers */
		std::cout << "time[s] max(freq)[MHz] sum(recloads) max(recloads) sum(loads) max(loads)\n";
		std::cout << std::fixed << std::setprecision(3);
	}

	void operator ()(std::atomic<bool> const * const die) {
		double statTime = 0.; /* in seconds */
		auto time = std::chrono::steady_clock::now();
		for (uint64_t interval;
		     !*die && (std::cin >> interval).good();) {
			/* align freqs to freq_levels */
			fixFreqs();
			/* reporting variables */
			double statSumRecloads = 0.;
			double statMaxRecloads = 0.;
			double statSumLoads = 0.;
			double statMaxLoads = 0.;
			int statMaxFreq = 0;
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
				if (!frameSum) {
					continue;
				}
				auto const coreFreq = this->freqs[core]->get<int>();
				auto const coreRefFreq = this->freqRefs[core];

				/* calc recorded stats */
				double load = double(frameLoad) / frameSum;
				statSumRecloads += load;
				statMaxRecloads = statMaxRecloads > load ? statMaxRecloads : load;

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
				load = double(frameLoad) / frameSum;
				statSumLoads += load;
				statMaxLoads = statMaxLoads > load ? statMaxLoads : load;
			}

			/* print stats */
			statTime += double(interval) / 1000;
			std::cout << statTime << ' ' << statMaxFreq << ' '
			          << statSumRecloads << ' ' << statMaxRecloads << ' '
			          << statSumLoads << ' ' << statMaxLoads << '\n';

			/* sleep */
			std::this_thread::sleep_until(time += ms{interval});

			/* commit changes */
			cp_times.set(&sum[0], this->size);
		}

		/* tell process to die */
		kill(getpid(), SIGINT);
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
		input = input.substr(input.find(' '));
		sysctls.addValue(std::string{CP_TIMES}, input);
		this->bgthread = std::thread{Emulator{}, &this->die};
	}

	~Main() {
		this->die = true;
		this->bgthread.join();
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
static fn_sysctlnametomib  orig_sysctlnametomib = nullptr;;

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
		*oldlenp = sysctls[mib].size();
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

	return 0;
} catch (std::out_of_range &) {
	return orig_sysctl(name, namelen, oldp, oldlenp, newp, newlen);
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
	return 0;
} catch (std::out_of_range &) {
	return orig_sysctlnametomib(name, mibp, sizep);
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
	return orig_sysctlbyname(name, oldp, oldlenp, newp, newlen);
}

int daemon(int, int) { return 0; }

uid_t geteuid(void) { return 0; }

pidfh * pidfile_open(const char *, mode_t, pid_t *) {
	return reinterpret_cast<pidfh *>(&pidfile_open);
}

int pidfile_write(pidfh *) { return 0; }

int pidfile_close(pidfh *) { return 0; }

int pidfile_remove(pidfh *) { return 0; }

int pidfile_fileno(pidfh const *) { return 0; }

} /* extern "C" */

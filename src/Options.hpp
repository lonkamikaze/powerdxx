/** \file
 * This file provides \ref nih::Options<>, a substitute for `getopt(3)`.
 *
 * The `getopt(3)` interface takes the command line arguments as `char * const`
 * instead of `char const *`. I.e. it reserves the right to mutate the
 * provided arguments, which it actually does.
 *
 * The \ref nih::Options<> functor is not a drop in substitute, but
 * tries to be easily adoptable and does not change the data given to it.
 *
 * To use the options an enum or enum class is required, e.g.:
 *
 * ~~~~{.cpp}
 * enum class MyOptions {
 * 	USAGE, FILE_IN, FILE_OUT, FLAG_VERBOSE,
 * 	OPT_UNKNOWN, OPT_NOOPT, OPT_DASH, OPT_LDASH, OPT_DONE
 * };
 * ~~~~
 *
 * The options prefixed with `OPT_` are obligatory. Their meaning is
 * documented in nih::enum_has_members<>. Their presence is validated
 * at compile time.
 *
 * The enum values are returned when selecting the next option, in order
 * to do that a usage string and a list of definitions are needed:
 *
 * ~~~~{.cpp}
 * static char const * const USAGE = "[-hv] [-i file] [-o file] [command ...]";
 *
 * static nih::Option<MyOptions> const OPTIONS[]{
 * 	{MyOptions::USAGE,        'h', "help",    "",     "Show this help"},
 * 	{MyOptions::USAGE,         0,  "usage",   "",     ""},
 * 	{MyOptions::FILE_IN,      'i', "in",      "file", "Input file"},
 * 	{MyOptions::FILE_OUT,     'o', "out",     "file", "Output file"},
 * 	{MyOptions::FLAG_VERBOSE, 'v', "verbose", "",     "Verbose output"}
 * };
 * ~~~~
 *
 * Every array entry defines an option consisting of the enum value that
 * represents it, a short and a long version (either of which are optional)
 * and a comma separated list of arguments. The final string appears in the
 * usage() output. The details are documented by nih::Option<>.
 *
 * Aliases are created by adding a definition that returns the same enum
 * value.
 *
 * For the short version it does not matter whether `-ifile` or `-i file` is
 * provided, the long version must be `--in file`. Short options without
 * arguments may be directly followed by another short option, e.g. `-vofile`
 * is equivalent to `-v -o file`.
 *
 * The option definitions should be passed to nih::make_Options() to
 * create the functor:
 *
 * ~~~~{.cpp}
 * #include <iostream>
 * ...
 * 
 * int main(int argc, char * argv[]) {
 * 	char const * infile = "-";
 * 	char const * outfile = "-";
 * 	bool verbose = false;
 * 
 * 	auto getopt = nih::make_Options(argc, argv, USAGE, OPTIONS);
 * 	while (true) switch (getopt()) { // get new option/argument
 * 	case MyOptions::USAGE:
 * 		std::cerr << getopt.usage(); // show usage
 * 		return 0;
 * 	case MyOptions::FILE_IN:
 * 		infile = getopt[1]; // get first argument
 * 		break;
 * 	case MyOptions::FILE_OUT:
 * 		outfile = getopt[1]; // get first argument
 * 		break;
 * 	case MyOptions::FLAG_VERBOSE:
 * 		verbose = true;
 * 		break;
 * 	case MyOptions::OPT_UNKNOWN:
 * 	case MyOptions::OPT_NOOPT:
 * 	case MyOptions::OPT_DASH:
 * 	case MyOptions::OPT_LDASH:
 * 		std::cerr << "Unexpected command line argument: "
 * 		          << getopt[0] << '\n'; // output option/argument
 * 		return 1;
 * 	case MyOptions::OPT_DONE:
 * 		return do_something(infile, outfile, verbose);
 * 	}
 * 	return 0;
 * }
 * ~~~~
 *
 * Every call of the functor moves on to the next option or argument.
 * For non-option arguments it returns `OPT_NOOPT`.
 *
 * The `getopt[1]` calls return the first argument following the option. It
 * is possible to retrieve more arguments than were defined in the options
 * definition. The `[]` opterator always returns a valid, terminated string
 * (provided the command line arguments are valid, terminated strings). So
 * it is always safe to dereference the pointer, even when reading beyond the
 * end of command line arguments.
 *
 * The `getopt[0]` calls return the command line argument that contains the
 * selected option. So in the `FILE_IN` case it could be any of `-i`, `--in`,
 * `-vi`, `-ifile` or `-vifile`. This is useful for the `OPT_UNKNOWN`
 * and `OPT_NOOPT` cases. The `getopt[1]` call on the other hand would always
 * return `file` regardless of argument chaining.
 */

#ifndef _POWERDXX_NIH_OPTIONS_HPP_
#define _POWERDXX_NIH_OPTIONS_HPP_

#include <cstddef>     /* size_t */
#include <iomanip>     /* std::left, std::setw */
#include <sstream>     /* std::ostringstream, std::string */
#include <type_traits> /* std::true_type */
#include <cassert>     /* assert() */

/**
 * Not invented here namespace, for code that substitutes already commonly
 * available functionality.
 */
namespace nih {

/**
 * See std::void_t in C++17 \<type_traits\>.
 */
template <class...>
using void_t = void;

/**
 * Tests whether the given enum provides all the required definitions.
 *
 * The Options<> template expects the provided enum to provide the
 * following members:
 *
 * | Member      | Description                                            |
 * |-------------|--------------------------------------------------------|
 * | OPT_UNKNOWN | An undefined option (long or short) was encountered    |
 * | OPT_NOOPT   | The encountered command line argument is not an option |
 * | OPT_DASH    | A single dash "-" was encountered                      |
 * | OPT_LDASH   | Double dashes "--" were encountered                    |
 * | OPT_DONE    | All command line arguments have been processed         |
 *
 * @tparam Enum
 *	An enum or enum class representing the available options
 */
template <class Enum, class = void>
struct enum_has_members : /** @cond */ std::false_type {};

template <class Enum>
struct enum_has_members<Enum, void_t<decltype(Enum::OPT_UNKNOWN),
                                     decltype(Enum::OPT_NOOPT),
                                     decltype(Enum::OPT_DASH),
                                     decltype(Enum::OPT_LDASH),
                                     decltype(Enum::OPT_DONE)>> :
    /** @endcond */ std::true_type {};

/**
 * Container for an option definition.
 *
 * Aliases can be defined by creating definitions with the same enumval
 * member.
 *
 * The lopt, args and usage members have to be 0 terminated, using string
 * literals is safe.
 *
 * @tparam Enum
 *	An enum or enum class representing the available options
 */
template <class Enum>
struct Option {
	/**
	 * The enum value to return for this option.
	 */
	Enum enumval;

	/**
	 * The short version of this option.
	 *
	 * Set to 0 if no short option is available.
	 */
	char sopt;

	/**
	 * The long version of this option.
	 *
	 * Set to nullptr or "" if no long option is available.
	 */
	char const * lopt;

	/**
	 * A comma separated list of arguments.
	 *
	 * Set to nullptr or "" if no argument is available.
	 */
	char const * args;

	/**
	 * A usage string.
	 */
	char const * usage;
};

/**
 * Retrieves the count of arguments in an option definition.
 *
 * @tparam Enum
 *	An enum or enum class representing the available options
 * @param def
 *	The option definition
 * @return
 *	The number of arguments specified in the given definition
 */
template <class Enum>
size_t argCount(Option<Enum> const & def) {
	if (!def.args || !def.args[0]) { return 0; }
	size_t argc = 1;
	for (char const * pch = def.args; *pch; ++pch) {
		argc += (*pch == ',' ? 1 : 0);
	}
	return argc;
}

/**
 * An instance of this class offers operators to retrieve command line
 * options and arguments.
 *
 * Instantiate with make_Options() to infer template parameters
 * automatically.
 *
 * Check the `operator ()` and `operator []` for use.
 *
 * @tparam Enum
 *	An enum or enum class matching the requirements set by
 *	enum_has_members
 * @tparam DefCount
 *	The number of option definitions
 */
template <class Enum, size_t DefCount>
class Options {
	static_assert(enum_has_members<Enum>::value,
	              "The enum must have the members OPT_UNKNOWN, OPT_NOOPT, OPT_DASH, OPT_LDASH and OPT_DONE");
	private:
	/**
	 * The number of command line arguments.
	 */
	int const argc;

	/**
	 * The command line arguments.
	 */
	char const * const * const argv;

	/**
	 * A string literal for the usage() output.
	 */
	char const * const usageStr;

	/**
	 * A reference to the option definitions.
	 */
	Option<Enum> const (& defs)[DefCount];

	/**
	 * The option definition to use for unknown options.
	 */
	Option<Enum> const opt_unknown{
		Enum::OPT_UNKNOWN, 0, nullptr, nullptr, nullptr
	};

	/**
	 * The option definition to use for non-options.
	 */
	Option<Enum> const opt_noopt{
		Enum::OPT_NOOPT, 0, nullptr, nullptr, nullptr
	};

	/**
	 * The option definition to use for a single dash.
	 */
	Option<Enum> const opt_dash{
		Enum::OPT_DASH, 0, nullptr, nullptr, nullptr
	};

	/**
	 * The option definition to use for a single double-dash.
	 */
	Option<Enum> const opt_ldash{
		Enum::OPT_LDASH, 0, nullptr, nullptr, nullptr
	};

	/**
	 * The index of the command line argument containing the current
	 * option.
	 */
	int argi;

	/**
	 * Points to the current short option character.
	 */
	char const * argp;

	/**
	 * Points to the current option definition.
	 */
	Option<Enum> const * current;

	/**
	 * Returns a pointer to the file name portion of the given string.
	 *
	 * @param file
	 *	The string containing the path to the file
	 * @return
	 *	A pointer to the file name portion of the path
	 */
	static char const * removePath(char const * const file) {
		auto result = file;
		for (auto ptr = file; *ptr; ++ptr) {
			if (*ptr == '/' || *ptr == '\\') {
				result = ptr + 1;
			}
		}
		return result;
	}

	/**
	 * Returns true if the given strings match.
	 *
	 * @param lstr,rstr
	 *	Two 0 terminated strings
	 * @retval true
	 *	The given strings match
	 * @retval false
	 *	The strings do not match
	 */
	static bool match(char const * const lstr, char const * const rstr) {
		size_t i = 0;
		for (; lstr[i] && rstr[i]; ++i) {
			if (lstr[i] != rstr[i]) { return false; }
		}
		return lstr[i] == rstr[i];
	}

	/**
	 * Returns true if one of the given strings matches the beginning
	 * of the other.
	 *
	 * @param lstr,rstr
	 *	Two 0 terminated strings
	 * @retval true
	 *	The shorter string matches the beginning of the other string
	 * @retval false
	 *	The strings do not match
	 */
	static bool bmatch(char const * const lstr, char const * const rstr) {
		for (size_t i = 0; lstr[i] && rstr[i]; ++i) {
			if (lstr[i] != rstr[i]) { return false; }
		}
		return true;
	}

	/**
	 * Finds the short option matching the given character.
	 *
	 * @param ch
	 *	The short option to find
	 * @return
	 *	An option definition by reference
	 */
	Option<Enum> const & get(char const ch) {
		if (!ch) {
			return this->opt_dash;
		}
		for (auto const & def : this->defs) {
			if (def.sopt == ch) {
				return def;
			}
		}
		return this->opt_unknown;
	}

	/**
	 * Finds the long option matching the given string.
	 *
	 * @param str
	 *	The long option to find
	 * @return
	 *	An option definition by reference
	 */
	Option<Enum> const &  get(char const * const str) {
		if (!str[0]) {
			return this->opt_ldash;
		}
		for (auto const & def : this->defs) {
			if (match(str, def.lopt)) {
				return def;
			}
		}
		return this->opt_unknown;
	}

	public:
	/**
	 * Construct an options functor.
	 *
	 * @param argc,argv
	 *	The command line arguments
	 * @param usage
	 *	A usage string following "usage: progname "
	 * @param defs
	 *	An array of option definitions
	 */
	Options(int const argc, char const * const * const argv,
	        char const * const usage,
	        Option<Enum> const (& defs)[DefCount]) :
	    argc{argc}, argv{argv}, usageStr{usage}, defs{defs},
	    argi{0}, argp{nullptr}, current{nullptr} {}

	/**
	 * Updates the internal state by parsing the next option.
	 *
	 * When reaching the end of the argument list, the internal
	 * state is reset, so a successive call will restart the
	 * argument parsing.
	 *
	 * @return
	 *	A self-reference
	 */
	Options & operator ()() {
		/*
		 * point argi and argp to the appropriate places
		 */
		if (this->current) {
			/* this is not the first call */
			if (this->argp && this->argp[0] && this->argp[1]) {
				/* argp is set and does not point to the end
				 * of an argument */
				if (argCount(*this->current) == 0) {
					/* proceed to the next short option */
					++this->argp;
				} else {
					/* the chained characters were an
					 * option argument */
					this->argp = nullptr;
					this->argi += argCount(*this->current);
				}
			} else {
				/* point forward for the option stand alone
				 * case */
				this->argp = nullptr;
				this->argi += argCount(*this->current) + 1;
			}
		} else {
			/* no current state, start with the first argument */
			this->argi = 1;
			this->argp = nullptr;
		}

		/*
		 * match the current option
		 */
		/* ran out of options */
		if (this->argi >= this->argc) {
			/* reset state */
			this->current = nullptr;
			return *this;
		}
		/* continue short option chain */
		if (this->argp) {
			this->current = &get(this->argp[0]);
			return *this;
		}
		/* long option */
		if (bmatch(this->argv[this->argi], "--")) {
			this->current = &get(this->argv[this->argi] + 2);
			return *this;
		}
		/* short option */
		if (this->argv[this->argi][0] == '-') {
			this->argp = this->argv[this->argi] + 1;
			this->current = &get(this->argp[0]);
			return *this;
		}
		/* not an option */
		this->current = &this->opt_noopt;
		return *this;
	}

	/**
	 * Implicitly cast to the current option.
	 *
	 * @return
	 *	An Enum member representing the current option
	 * @retval OPT_UNKNOWN
	 *	An option that was not in the list of option definitions
	 *	was encountered
	 * @retval OPT_NOOPT
	 *	An argument that is not an option was encountered
	 * @retval OPT_DASH
	 *	A lone dash "-" was encountered
	 * @retval OPT_LDASH
	 *	A lone long dash "--" was encountered
	 * @retval OPT_DONE
	 *	All arguments have been processed, or argument processing
	 *	has not yet started
	 */
	operator Enum() const {
		return this->current ? this->current->enumval : Enum::OPT_DONE;
	}

	/**
	 * Retrieve arguments to the current option.
	 *
	 * The string containing the current option is returned with i = 0,
	 * the arguments following the option with greater values of i.
	 *
	 * When no more arguments are left the empty string is returned.
	 *
	 * @param i
	 *	The index of the argument to retrieve
	 * @return
	 *	The option or one of its arguments
	 */
	char const * operator [](int const i) const {
		/* argument is in the same string as option */
		if (this->argp && this->argp[0] && this->argp[1] && i > 0) {
			if (this->argi + i - 1 >= this->argc) {
				return "";
			}
			if (i == 1) {
				return this->argp + 1;
			}
			return this->argv[this->argi + i - 1];
		}
		/* read in front of arguments */
		if (this->argi + i < 0) {
			return "";
		}
		/* argument is in the string following the option */
		if (this->argi + i < this->argc) {
			return this->argv[this->argi + i];
		}
		/* read beyond end of arguments */
		return "";
	}

	/**
	 * Returns a string for usage output, created from the option
	 * definitions.
	 *
	 * @return
	 *	A usage string for printing on the CLI
	 */
	std::string usage() const {
		std::ostringstream result;
		result << "usage: " << removePath(this->argv[0]) << ' '
		       << this->usageStr << "\n\n" << std::left;

		/* collect options and arguments in arrays because formatting
		 * is only possible after all of them have been read */
		std::string options[DefCount];
		std::string arguments[DefCount];
		size_t options_max = 0;   /* length of the longest option */
		size_t arguments_max = 0; /* length of the longest argument */
		size_t i = 0;             /* current array index */
		for (auto const & def : this->defs) {
			/* get option */
			if (def.sopt && def.lopt && def.lopt[0]) {
				options[i] = std::string{'-'} + def.sopt +
				             ", --" + def.lopt;
			} else if (def.sopt) {
				options[i] = std::string{'-'} + def.sopt;
			} else if (def.lopt && def.lopt[0]) {
				options[i] = std::string{"    --"} + def.lopt;
			}
			options_max = options[i].length() > options_max ?
			              options[i].length() : options_max;
			/* get arguments */
			arguments[i] = def.args;
			for (char & ch : arguments[i]) {
				ch = (ch == ',' ? ' ' : ch);
			}
			arguments_max = arguments[i].length() > arguments_max ?
			                arguments[i].length() : arguments_max;
			++i;
		}
		/* assemble all the definitions into the string with proper
		 * column widths */
		for (size_t i = 0; i < DefCount; ++i) {
			result << "  " << std::setw(options_max) << options[i]
			       << "  " << std::setw(arguments_max)
			                 << arguments[i]
			       << "  " << this->defs[i].usage << '\n';
		}
		return result.str();
	}

	/**
	 * Provide a string containing the entire command line, with the
	 * indexed argument highlighted.
	 *
	 * The current implementation highlights arguments by underlining
	 * them with `^~~~`.
	 *
	 * @param i
	 *	The argument index, like operator []
	 * @param n
	 *	The number of arguments to highlight, highlights all
	 *	remaining arguments if n <= 0
	 * @return
	 *	A string formatted to highlight the given argument
	 */
	std::string show(int const i, int const n = 1) const {
		/* select a whole argument */
		char const * const select = (*this)[i];
		/* if the current option (i == 0) is requested, pick
		 * up the pointer to the current short option character */
		char const * const argp = i == 0 ? this->argp : nullptr;
		using std::string;
		string cmd;       /* command and arguments string */
		string ul;        /* underlining string */
		int hilight = 0;  /* #args left to underline */
		/* build cmd and ul string */
		for (size_t p = 0; p < this->argc; ++p) {
			/* build each argument character wise */
			for (auto it = this->argv[p];; ++it) {
				/* underlining character */
				char ulc = hilight ? '~' : ' ';
				/* underline short option */
				if (argp && it == argp) {
					hilight = n > 0 ? n - 1 : -1;
					ulc = '^';
				}
				/* underline long option / argument */
				if (!argp && it == select) {
					hilight = n > 0 ? n : -1;
					ulc = '^';
				}
				/* add current character,
				 * continue to stay in the loop */
				switch (*it) {
				case 0:
					/* end of argument,
					 * add a space behind the argument */
					cmd += ' ';
					/* underline only if it's the first
					 * character to underline */
					ul += ulc == '^' ? '^' : ' ';
					break;
				case '\t':
					/* symbolic tab */
					cmd += "\\t";
					ul += ulc += ulc;
					continue;
				case '\n':
					/* symbolic newline */
					cmd += "\\n";
					ul += ulc += ulc;
					continue;
				case ' ':
				case '\\':
					/* escape */
					cmd += '\\';
					ul += ulc;
					/*[[fallthrough]];*/
				default:
					/* regular characer */
					cmd += *it;
					ul += ulc;
					continue;
				}
				break;
			}
			hilight > 0 && --hilight;
		}
		/* the selected option must be behind the command,
		 * e.g. because the last parameter is missing an argument */
		if (this->argi >= this->argc) {
			cmd += ' ';
			ul += '^';
		}
		return cmd + '\n' + ul;
	}

	/**
	 * Returns the argument offset of the current parameter/argument.
	 *
	 * @warning
	 *	This may return a value >= argc if the current state is
	 *	Enum::OPT_DONE
	 * @return
	 *	The current argument index
	 */
	int offset() const {
		return this->argi;
	}
};

/**
 * Wrapper around the Options<> constructor, that uses function template
 * matching to deduce template arguments.
 *
 * @tparam Enum
 *	An enum for all the available options
 * @tparam DefCount
 *	The number of option definitions
 * @param argc,argv
 *	The command line arguments
 * @param usage
 *	A usage string that is used in the header of the usage output
 * @param defs
 *	An array of option definitions
 */
template <class Enum, size_t DefCount>
constexpr Options<Enum, DefCount>
make_Options(int const argc, char const * const * const argv,
             char const * const usage, Option<Enum> const (&defs)[DefCount]) {
	return Options<Enum, DefCount>{argc, argv, usage, defs};
}

} /* namespace nih */

#endif /* _POWERDXX_NIH_OPTIONS_HPP_ */

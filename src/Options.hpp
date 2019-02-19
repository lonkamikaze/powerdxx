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
 * The enum values are returned when matching the next argument to
 * a parameter. In order to do that a usage string and a list of
 * parameter definitions are required:
 *
 * ~~~~{.cpp}
 * static char const * const USAGE = "[-hv] [-i file] [-o file] [command ...]";
 *
 * static nih::Parameter<MyOptions> const PARAMETERS[]{
 * 	{MyOptions::USAGE,        'h', "help",    "",     "Show this help"},
 * 	{MyOptions::USAGE,         0,  "usage",   "",     ""},
 * 	{MyOptions::FILE_IN,      'i', "in",      "file", "Input file"},
 * 	{MyOptions::FILE_OUT,     'o', "out",     "file", "Output file"},
 * 	{MyOptions::FLAG_VERBOSE, 'v', "verbose", "",     "Verbose output"}
 * };
 * ~~~~
 *
 * Each entry in the array defines a parameter consisting of the following:
 *
 * | Field    | Meaning                                           |
 * |----------|---------------------------------------------------|
 * | `option` | The option symbol (enum value)                    |
 * | `sparam` | An optional parameter character (short parameter) |
 * | `lparam` | An optional long parameter string                 |
 * | `args`   | A comma separated list of parameter arguments     |
 * | `usage`  | A descriptive string                              |
 *
 * Multiple parameters may be mapped to a single option (e.g. `--help`
 * and `--usage`). Parameters without arguments are called flags. It
 * is possible to map parameters with different numbers of arguments
 * to a single option, but this is arguably semantically confusing
 * and should not be done.
 *
 * Multiple flags' parameter characters can be concatenated in an argument.
 * A parameter with arguments' character can appear at the end of a
 * character chain. The first argument to the parameter may be concatenated
 * as well. E.g. `-v -i file`, `-vi file` and `-vifile` are all equivalent.
 * Parameters' string representations always stand alone, they can
 * neither be combined with each other nor with parameter characters.
 * E.g. `--verbose --in file` is the equivalent parameter string
 * representation.
 *
 * The usage string and the parameter usage strings are used to assemble
 * the string provided by the nih::Options<>::usage() method.
 *
 * The parameter definitions should be passed to nih::make_Options() to
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
 * 	auto getopt = nih::make_Options(argc, argv, USAGE, PARAMETERS);
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
 * Every call of the functor moves on to the next parameter or argument.
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
 * and `OPT_NOOPT` cases. The `getopt[1]` call on the other hand would
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
 * @tparam OptionT
 *	An enum or enum class representing the available options
 */
template <class OptionT, class = void>
struct enum_has_members : /** @cond */ std::false_type {};

template <class OptionT>
struct enum_has_members<OptionT, void_t<decltype(OptionT::OPT_UNKNOWN),
                                        decltype(OptionT::OPT_NOOPT),
                                        decltype(OptionT::OPT_DASH),
                                        decltype(OptionT::OPT_LDASH),
                                        decltype(OptionT::OPT_DONE)>> :
    /** @endcond */ std::true_type {};

/**
 * Container for an option definition.
 *
 * Aliases can be defined by creating definitions with the same option
 * member.
 *
 * The lparam, args and usage members have to be 0 terminated, using string
 * literals is safe.
 *
 * @tparam OptionT
 *	An enum or enum class representing the available options
 */
template <class OptionT>
struct Parameter {
	/**
	 * The enum value to return for this option.
	 */
	OptionT option;

	/**
	 * The short version of this parameter.
	 *
	 * Set to 0 if no short parameter is available.
	 */
	char sparam;

	/**
	 * The long version of this parameter.
	 *
	 * Set to nullptr or "" if no long parameter is available.
	 */
	char const * lparam;

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
 * @tparam OptionT
 *	An enum or enum class representing the available options
 * @param def
 *	The parameter definition
 * @return
 *	The number of arguments specified in the given definition
 */
template <class OptionT>
size_t argCount(Parameter<OptionT> const & def) {
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
 * @tparam OptionT
 *	An enum or enum class matching the requirements set by
 *	enum_has_members
 * @tparam DefCount
 *	The number of option definitions
 */
template <class OptionT, size_t DefCount>
class Options {
	static_assert(enum_has_members<OptionT>::value,
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
	Parameter<OptionT> const (& defs)[DefCount];

	/**
	 * The option definition to use for unknown options.
	 */
	Parameter<OptionT> const opt_unknown{
		OptionT::OPT_UNKNOWN, 0, nullptr, nullptr, nullptr
	};

	/**
	 * The option definition to use for non-options.
	 */
	Parameter<OptionT> const opt_noopt{
		OptionT::OPT_NOOPT, 0, nullptr, nullptr, nullptr
	};

	/**
	 * The option definition to use for a single dash.
	 */
	Parameter<OptionT> const opt_dash{
		OptionT::OPT_DASH, 0, nullptr, nullptr, nullptr
	};

	/**
	 * The option definition to use for a single double-dash.
	 */
	Parameter<OptionT> const opt_ldash{
		OptionT::OPT_LDASH, 0, nullptr, nullptr, nullptr
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
	Parameter<OptionT> const * current;

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
	Parameter<OptionT> const & get(char const ch) {
		if (!ch) {
			return this->opt_dash;
		}
		for (auto const & def : this->defs) {
			if (def.sparam == ch) {
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
	Parameter<OptionT> const &  get(char const * const str) {
		if (!str[0]) {
			return this->opt_ldash;
		}
		for (auto const & def : this->defs) {
			if (match(str, def.lparam)) {
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
	 *	An array of parameter definitions
	 */
	Options(int const argc, char const * const * const argv,
	        char const * const usage,
	        Parameter<OptionT> const (& defs)[DefCount]) :
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
	 *	An OptionT member representing the current option
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
	operator OptionT() const {
		return this->current
		       ? this->current->option
		       : OptionT::OPT_DONE;
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

		/* collect parameters and arguments in arrays because
		 * formatting is only possible after all of them have
		 * been read */
		std::string params[DefCount];
		std::string args[DefCount];
		size_t params_max = 0;  /* length of the longest option */
		size_t args_max = 0;    /* length of the longest argument */
		size_t i = 0;           /* current array index */
		for (auto const & def : this->defs) {
			/* get option */
			if (def.sparam && def.lparam && def.lparam[0]) {
				params[i] = std::string{'-'} + def.sparam +
				            ", --" + def.lparam;
			} else if (def.sparam) {
				params[i] = std::string{'-'} + def.sparam;
			} else if (def.lparam && def.lparam[0]) {
				params[i] = std::string{"    --"} + def.lparam;
			}
			params_max = params[i].length() > params_max
			             ? params[i].length() : params_max;
			/* get arguments */
			args[i] = def.args;
			for (char & ch : args[i]) {
				ch = (ch == ',' ? ' ' : ch);
			}
			args_max = args[i].length() > args_max
			           ? args[i].length() : args_max;
			++i;
		}
		/* assemble all the definitions into the string with proper
		 * column widths */
		for (size_t i = 0; i < DefCount; ++i) {
			result << "  " << std::setw(params_max) << params[i]
			       << "  " << std::setw(args_max) << args[i]
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
	 *	OptionT::OPT_DONE
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
 * @tparam OptionT
 *	An enum for all the available options
 * @tparam DefCount
 *	The number of option definitions
 * @param argc,argv
 *	The command line arguments
 * @param usage
 *	A usage string that is used in the header of the usage output
 * @param defs
 *	An array of parameter definitions
 */
template <class OptionT, size_t DefCount>
constexpr Options<OptionT, DefCount>
make_Options(int const argc, char const * const * const argv,
             char const * const usage,
             Parameter<OptionT> const (&defs)[DefCount]) {
	return Options<OptionT, DefCount>{argc, argv, usage, defs};
}

} /* namespace nih */

#endif /* _POWERDXX_NIH_OPTIONS_HPP_ */

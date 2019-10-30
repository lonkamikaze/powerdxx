/** \file
 * Implements c++ wrappers for \<cstdio> I/O functionality.
 */

#ifndef _POWERDXX_SYS_IO_HPP_
#define _POWERDXX_SYS_IO_HPP_

#include <cstdio>       /* fopen(), fprintf() etc. */

namespace sys {

/**
 * This namespace contains c++ wrappers for \<cstdio> functionality.
 */
namespace io {

/**
 * Feature flags for file type composition.
 *
 * @see	file_feature
 */
enum class feature {
	/**
	 * The file type supports read operations.
	 *
	 * @see file_feature<FileT, read, Tail ...>
	 */
	read,

	/**
	 * The file type supports write operations.
	 *
	 * @see file_feature<FileT, write, Tail ...>
	 */
	write,

	/**
	 * The file type supports seek operations.
	 *
	 * @see file_feature<FileT, seek, Tail ...>
	 */
	seek,
};

/** @copydoc feature::read */
static constexpr auto const read = feature::read;
/** @copydoc feature::write */
static constexpr auto const write = feature::write;
/** @copydoc feature::seek */
static constexpr auto const seek = feature::seek;

/**
 * Ownership relation to the underlying FILE object.
 */
enum class ownership {
	/**
	 * The file instance owns the FILE object.
	 *
	 * @see file<own, Features ...>
	 */
	own,

	/**
	 * The file instance refers to a FILE object managed somewhere else.
	 *
	 * @see file<link, Features ...>
	 */
	link,
};

/** @copydoc ownership::own */
static constexpr auto const own = ownership::own;
/** @copydoc ownership::link */
static constexpr auto const link = ownership::link;

/**
 * Produces file access types around the C file handling facilities.
 *
 * @tparam Ownership
 *	Determine the ownership relationship to the underlying FILE object
 * @tparam Features
 *	A list of features the file type supports
 * @see ownership
 * @see feature
 * @see file<own, Features ...>
 * @see file<link, Features ...>
 * @see file_feature
 */
template <ownership Ownership, feature ... Features> class file;

/**
 * Similar to std::enable_if, but it also has the value of the expression.
 *
 * @tparam T
 *	The return type if the expression is true
 */
template <bool, class T = void> struct enable_if {
	/**
	 * Provide the requested type.
	 */
	using type = T;

	/**
	 * The given expression is true.
	 */
	static constexpr bool const value{true};
};

/**
 * Specialise enable_if for a false expression.
 *
 * @tparam T
 *	The return type if the expression was true
 */
template <class T> struct enable_if<false, T> {
	/**
	 * The given expression is false.
	 */
	static constexpr bool const value{false};
};

/**
 * Pack a set of integral values in a type.
 *
 * @tparam Set
 *	A set of integral values
 */
template <auto ... Set> struct set {};

/**
 * Check whether a set type contains a value.
 *
 * @tparam SetT
 *	A set of integral values packed in io::set
 * @tparam Value
 *	The value to look up
 */
template <class SetT, auto Value> struct contains;

/**
 * Specialise io::contains to unpack io::set.
 *
 * @tparam Set
 *	The set of integral values to search
 * @tparam Value
 *	The value to find in Set
 */
template <auto ... Set, auto Value>
struct contains<set<Set ...>, Value> : enable_if<((Set == Value) || ...)> {};

/** @copydoc contains */
template <class SetT, auto Value>
constexpr auto const contains_v = contains<SetT, Value>::value;

/** @copydoc contains */
template <class SetT, auto Value>
using contains_t = typename contains<SetT, Value>::type;

/**
 * Check whether the left hand set is a superest of the right hand set.
 *
 * @tparam LSetT,RSetT
 *	Two io::set instances
 */
template <class LSetT, class RSetT> struct is_superset_of;

/**
 * Specialise is_superset_of to unpack the right hand io::set.
 *
 * @tparam LSetT
 *	The left hand io::set
 * @tparam RSet
 *	The right hand io::set values
 */
template <class LSetT, auto ... RSet>
struct is_superset_of<LSetT, set<RSet ...>> :
    enable_if<(contains_v<LSetT, RSet> && ...)> {};

/** @copydoc is_superset_of */
template <class LSetT, class RSetT>
constexpr auto const is_superset_of_v = is_superset_of<LSetT, RSetT>::value;

/** @copydoc is_superset_of */
template <class LSetT, class RSetT>
using is_superset_of_t = typename is_superset_of<LSetT, RSetT>::type;

/**
 * Ask questions about the contents of a string.
 */
struct query {
	/**
	 * Test a string whether it contains a set of characters.
	 */
	struct contains_ftor {
		/**
		 * The string to ask about.
		 */
		char const * const str;

		/**
		 * Check for a single character.
		 *
		 * @param ch
		 *	The character to check for
		 * @return
		 *	Whether the given character is part of the string
		 */
		constexpr bool operator ()(char const ch) const {
			for (auto it = str; it && *it; ++it) {
				if (*it == ch) {
					return true;
				}
			}
			return false;
		}

		/**
		 * Check for a set of characters if at least one is part of
		 * the string.
		 *
		 * @tparam CharTs
		 *	The character types
		 * @param chars
		 *	The set of characters
		 * @return
		 *	Whether at least one of the given characters
		 *	is in the string
		 */
		template <typename ... CharTs>
		constexpr bool any(CharTs const ... chars) const {
			for (auto it = str; it && *it; ++it) {
				if (((*it == chars) || ...)) {
					return true;
				}
			}
			return false;
		}

		/**
		 * Check for a set of characters if all of them are
		 * part of the string.
		 *
		 * @tparam CharTs
		 *	The character types
		 * @param chars
		 *	The set of characters
		 * @return
		 *	Whether all of the given characters are in
		 *	the string
		 */
		template <typename ... CharTs>
		constexpr bool all(CharTs const ... chars) const {
			for (auto ch : {chars ...}) {
				bool match{false};
				for (auto it = str; it && *it && !match; ++it) {
					match = (ch == *it);
				}
				if (!match) {
					return false;
				}
			}
			return true;
		}
	} const contains; /**< Query the string for characters. */
};

/**
 * Implements the base functionality of all file access types.
 *
 * @tparam FileT
 *	The file access type inheriting the feature
 * @see file_feature<FileT, read, Tail ...>
 * @see file_feature<FileT, write, Tail ...>
 * @see file_feature<FileT, seek, Tail ...>
 */
template <class FileT, feature ...>
class file_feature {
	protected:
	/**
	 * A pointer to the underlying FILE object.
	 */
	FILE * handle;

	/**
	 * Implicit cast up to inheriting file access type.
	 */
	operator FileT &() {
		return *static_cast<FileT *>(this);
	}

	/**
	 * Construct from a FILE object pointer.
	 *
	 * @param handle
	 *	A pointer to the object keeping file descriptor state
	 */
	file_feature(FILE * const handle) : handle{handle} {}

	public:
	/**
	 * Cast to boolean.
	 *
	 * @see feof()
	 * @see ferror()
	 * @retval true
	 *	The file instance point to a FILE object, which is
	 *	not in EOF or error state
	 * @retval false
	 *	The file instance does not point to a FILE object,
	 *	or the FILE object is in EOF or error state
	 */
	explicit operator bool() const {
		return this->handle && !feof(this->handle) &&
		       !ferror(this->handle);
	}

	/**
	 * Return whether the file instance is in EOF state.
	 *
	 * @see feof()
	 * @return
	 *	Whether the file instance points to a FILE object
	 *	and is in EOF state
	 */
	bool eof() const {
		return this->handle && feof(this->handle);
	}

	/**
	 * Return whether the file instance is in an error state.
	 *
	 * @see ferror()
	 * @return
	 *	Whether the file instance points to a FILE object
	 *	and is in an error state
	 */
	bool error() const {
		return this->handle && ferror(this->handle);
	}
};

/**
 * Implement read support for file types.
 *
 * @tparam FileT
 *	The file access type inheriting the feature
 * @tparam Tail
 *	The remaining features
 */
template <class FileT, feature ... Tail>
class file_feature<FileT, read, Tail ...> :
    public file_feature<FileT, Tail ...> {
	public:
	using file_feature<FileT, Tail ...>::file_feature;

	/**
	 * Read a single character from the file.
	 *
	 * @see fgetc()
	 * @return
	 *	The character or EOF
	 */
	int getc() {
		if (this->handle) {
			return fgetc(this->handle);
		}
		return EOF;
	}

	/**
	 * Read the given object from the file.
	 *
	 * @see fread()
	 * @tparam T
	 *	The object type, should be a POD type
	 * @param dst
	 *	A reference to the object to overwrite
	 * @return
	 *	The number of characters read
	 */
	template <typename T>
	std::size_t read(T & dst) {
		if (this->handle) {
			return fread(&dst, sizeof(T), 1, this->handle);
		}
		return 0;
	}

	/**
	 * Read the requested number of objects from the file.
	 *
	 * @see fread()
	 * @tparam T
	 *	The object type, should be a POD type
	 * @tparam Count
	 *	The number of objects in the destination buffer
	 * @param dst
	 *	A reference to an array of objects
	 * @param count
	 *	The number of objects to read
	 * @return
	 *	The number of characters read
	 */
	template <typename T, std::size_t Count>
	std::size_t read(T (& dst)[Count], std::size_t const count) {
		if (this->handle) {
			return fread(&dst, sizeof(T),
			             (count <= Count ? count : Count),
			             this->handle);
		}
		return 0;
	}

	/**
	 * Read formatted input.
	 *
	 * @see fscanf()
	 * @tparam CountFmt
	 *	The number of characters in the format string
	 * @tparam RefTs
	 *	The argument types to read
	 * @param fmt
	 *	The input format
	 * @param refs
	 *	A set of references to write to
	 * @return
	 *	The number of inputs successfully parsed
	 * @retval EOF
	 *	No inputs could be parsed due to end of file
	 */
	template <auto CountFmt, typename ... RefTs>
	int scanf(char const (& fmt)[CountFmt], RefTs & ... refs) {
		if (this->handle) {
			return fscanf(this->handle, fmt, &refs ...);
		}
		return EOF;
	}

	/**
	 * Read a line from the file.
	 *
	 * Reads the file up to and including the first newline or
	 * terminating zero, as long as it fits into the destination
	 * buffer. Always zero terminated.
	 *
	 * @see fgets()
	 * @tparam Count
	 *	The maximum number of characters to read
	 * @param dst
	 *	A reference to the destination buffer
	 * @retval true
	 *	Characters have been read
	 * @retval false
	 *	Characters could not be read
	 */
	template <auto Count>
	bool gets(char (& dst)[Count]) {
		return this->handle && fgets(dst, Count, this->handle);
	}
};

/**
 * Implement write support for file types.
 *
 * @tparam FileT
 *	The file access type inheriting the feature
 * @tparam Tail
 *	The remaining features
 */
template <class FileT, feature ... Tail>
class file_feature<FileT, write, Tail ...> :
    public file_feature<FileT, Tail ...> {
	public:
	using file_feature<FileT, Tail ...>::file_feature;

	/**
	 * Output with printf style formatting.
	 *
	 * @see fprintf()
	 * @tparam CountFmt
	 *	The number of characters in the formatting string
	 * @tparam ArgTs
	 *	The argument types of the data to print
	 * @param fmt
	 *	The format string
	 * @param args
	 *	The set of data to print
	 * @return
	 *	A self reference
	 */
	template <auto CountFmt, typename ... ArgTs>
	FileT & printf(char const (& fmt)[CountFmt], ArgTs const & ... args) {
		if (this->handle) {
			fprintf(this->handle, fmt, args ...);
		}
		return *this;
	}

	/**
	 * Output a printf style formatted string.
	 *
	 * This overload exists as a workaround for a bug in clang++-8's
	 * -Wformat-security that does not recognise the format as
	 * a literal string if no arguments follow.
	 *
	 * @see fprintf()
	 * @tparam CountFmt
	 *	The number of characters in the formatting string
	 * @param fmt
	 *	The format string
	 * @return
	 *	A self reference
	 */
	template <auto CountFmt>
	FileT & printf(char const (& fmt)[CountFmt]) {
		if (this->handle) {
			fprintf(this->handle, fmt, 0);
		}
		return *this;
	}

	/**
	 * Print an unformatted string, excluding the last character.
	 *
	 * This method is built around the assumption that the argument
	 * is a string literal and the last character is a terminating
	 * zero.
	 *
	 * @see fwrite()
	 * @tparam Count
	 *	The number of characters in the string
	 * @param msg
	 *	The string to print
	 * @return
	 *	A self reference
	 */
	template <std::size_t Count>
	FileT & print(char const (& msg)[Count]) {
		if (this->handle) {
			fwrite(msg, sizeof(char), Count - 1, this->handle);
		}
		return *this;
	}

	/**
	 * Write a single character to the string.
	 *
	 * @see fputc()
	 * @param character
	 *	The character to write
	 * @return
	 *	A self reference
	 */
	FileT & putc(int const character) {
		if (this->handle) {
			fputc(character, this->handle);
		}
		return *this;
	}

	/**
	 * Write an object to file.
	 *
	 * @see fwrite()
	 * @tparam T
	 *	The object type, should be a POD type
	 * @param src
	 *	The object to write out to the file
	 * @return
	 *	A self reference
	 */
	template <typename T>
	FileT & write(T const & src) {
		if (this->handle) {
			fwrite(&src, sizeof(T), 1, this->handle);
		}
		return *this;
	}

	/**
	 * Write an objects to file.
	 *
	 * @see fwrite()
	 * @tparam T
	 *	The object type, should be a POD type
	 * @tparam Count
	 *	The number of objects in the source buffer
	 * @param src
	 *	The object to write out to the file
	 * @param count
	 *	The number of objects to write
	 * @return
	 *	A self reference
	 */
	template <typename T, std::size_t Count>
	FileT & write(T const (& src)[Count], std::size_t const count) {
		if (this->handle) {
			fwrite(&src, sizeof(T),
			       (count <= Count ? count : Count),
			       this->handle);
		}
		return *this;
	}

	/**
	 * Flush file buffers.
	 *
	 * @see fflush()
	 * @return
	 *	A self reference
	 */
	FileT & flush() {
		if (this->handle) {
			fflush(this->handle);
		}
		return *this;
	}
};

/**
 * Implement seek support for file types.
 *
 * @tparam FileT
 *	The file access type inheriting the feature
 * @tparam Tail
 *	The remaining features
 */
template <class FileT, feature ... Tail>
class file_feature<FileT, seek, Tail ...> :
    public file_feature<FileT, Tail ...> {
	public:
	using file_feature<FileT, Tail ...>::file_feature;

	/**
	 * Seek file position.
	 *
	 * @see fseek()
	 * @param offset
	 *	The origin relative file position for binary files
	 *	or an absolute position returned by tell() for text
	 *	files
	 * @param origin
	 *	One of SEEK_SET, SEEK_CUR, SEEK_END
	 * @return
	 *	A self reference
	 */
	FileT & seek(long int const offset, int const origin) {
		if (this->handle) {
			fseek(this->handle, offset, origin);
		}
		return *this;
	}

	/**
	 * Reset file position to the beginning of the file.
	 *
	 * @see frewind()
	 * @return
	 *	A self reference
	 */
	FileT & rewind() {
		if (this->handle) {
			frewind(this->handle);
		}
		return *this;
	}

	/**
	 * Retrieve the current file position.
	 *
	 * @see ftell()
	 * @return
	 *	The current file offset
	 */
	long int tell() {
		if (this->handle) {
			return ftell(this->handle);
		}
		return -1;
	}
};

/**
 * Specialise for FILE object owning file instances.
 *
 * Ownership implies some semantics:
 *
 * - Offers a constructor that opens a file
 * - Cannot be copy constructed/assigned
 * - Can be move constructed/assigned from other owning file instances
 * - Can close()
 * - Implicit close() when going out of scope
 *
 * @tparam Features
 *	The set of file access features to support
 */
template <feature ... Features>
class file<own, Features ...> final :
    public file_feature<file<own, Features ...>, Features ...> {
	public:
	/**
	 * Must not copy construct for risk of multiple close() on
	 * the same file.
	 */
	file(file const &) = delete;

	/**
	 * Move construct from a temporary.
	 *
	 * @param move
	 *	The rvalue file to acquire the FILE object from
	 */
	file(file && move) : file{move.release()} {}

	/**
	 * Take ownership of the given FILE object.
	 *
	 * This can be used to take ownership of FILE objects provided
	 * by a legacy C interface.
	 *
	 * @param handle
	 *	A pointer to a FILE object
	 */
	explicit file(FILE * const handle) :
	    file_feature<file, Features ...>{handle} {}

	/**
	 * Move construct from another owning file type instance.
	 *
	 * The origin file type instance must support all features
	 * supported by this file type.
	 *
	 * @tparam Superset
	 *	The feature set of the original FILE object owner
	 * @param move
	 *	The rvalue file to acquire the FILE object from
	 */
	template <feature ... Superset,
	          class = is_superset_of_t<set<Superset ...>,
	                                   set<Features ...>>>
	file(file<own, Superset ...> && move) :
	    file{move.release()} {}

	/**
	 * Open a file by name.
	 *
	 * Failure to open a file occurs silently, but can be detected
	 * by boolean checking the file instance.
	 *
	 * The arguments of this constructor are forwarded to fopen(),
	 * provided the mode argument does not contradict the feature
	 * set of this file type.
	 *
	 * It is recommended to always add the 'b' (binary) character
	 * to the mode string, because text mode behaves quirkily.
	 *
	 * The feature::seek feature is not supported with 'a' (append),
	 * it is available with 'a+', but it behaves quirkily. Which
	 * means two different files of the same type may have different
	 * seek behaviour, depending on how the file was opened.
	 * Refer to the fopen() spec for the unsettling details.
	 * 
	 * @see fopen()
	 * @param filename
	 *	The name of the file
	 * @param mode
	 *	The file access mode, must not contradict the feature
	 *	set of this file type
	 */
	file(char const * const filename, char const * const mode) :
	    file{(!filename || !mode ||
	          (contains_v<set<Features ...>, write> &&
		   !query{mode}.contains.any('w', '+')) ||
		  (contains_v<set<Features ...>, read> &&
		   !query{mode}.contains.any('r', '+')) ||
		  (contains_v<set<Features ...>, seek> &&
		   query{mode}.contains('a') && !query{mode}.contains('+')))
	         ? nullptr : fopen(filename, mode)} {}

	/**
	 * Free all resources.
	 */
	~file() {
		close();
	}

	/**
	 * Move assign from another owning file type instance.
	 *
	 * The origin file type instance must support all features
	 * supported by this file type.
	 *
	 * @tparam Superset
	 *	The feature set of the original FILE object owner
	 * @param move
	 *	The rvalue file to acquire the FILE object from
	 * @return
	 *	A self reference
	 */
	template <feature ... Superset,
	          class = is_superset_of_t<set<Superset ...>, set<Features ...>>>
	file & operator =(file<own, Superset ...> && move) {
		~file();
		this->handle = move.release();
		return *this;
	}

	/**
	 * Provide the internal FILE object pointer.
	 *
	 * Can be used to pass the file to legacy C interfaces.
	 *
	 * @return
	 *	A pointer to the managed FILE object
	 */
	FILE * get() const {
		return this->handle;
	}

	/**
	 * Surrender ownership of the internal FILE object pointer.
	 *
	 * Can be used to pass the file to legacy C interfaces.
	 *
	 * @return
	 *	A pointer to the managed FILE object
	 */
	FILE * release() {
		auto const handle = this->handle;
		this->handle = nullptr;
		return handle;
	}

	/**
	 * Close the file.
	 *
	 * @return
	 *	A self reference
	 */
	file & close() {
		if (this->handle) {
			fclose(this->handle);
			this->handle = nullptr;
		}
		return *this;
	}

};

/**
 * Specialise for FILE object linking file instances.
 *
 * Lack of ownership implies some semantics:
 *
 * - Cannot be used to open files
 * - Can be copy constructed/assigned from other owning and non-owning
 *   file instances
 * - Cannot be move constructed/assigned from owning file instances
 * - Cannot close()
 *
 * @tparam Features
 *	The set of file access features to support
 */
template <feature ... Features>
class file<link, Features ...> final :
    public file_feature<file<link, Features ...>, Features ...> {
	public:
	/**
	 * Use the given FILE object.
	 *
	 * This can be used to refer to FILE objects managed by legacy
	 * C code.
	 *
	 * @param handle
	 *	A pointer to a FILE object
	 */
	explicit file(FILE * const handle) :
	    file_feature<file, Features ...>{handle} {}

	/**
	 * Copy construct from another file type instance.
	 *
	 * The origin file type instance must support all features
	 * supported by this file type.
	 *
	 * @tparam Ownership
	 *	The ownership status of the other file type
	 * @tparam Superset
	 *	The feature set of another file type
	 * @tparam Cond
	 *	Whether Superset is an actual superset of Features
	 * @param copy
	 *	The lvalue file to acquire the FILE object from
	 */
	template <ownership Ownership, feature ... Superset,
	          class = is_superset_of_t<set<Superset ...>, set<Features ...>>>
	file(file<Ownership, Superset ...> const & copy) : file{copy.get()} {}

	/**
	 * Must not move construct from files with ownership of their handle.
	 *
	 * @tparam Superset
	 *	The feature set of another FILE object owning file type
	 */
	template <feature ... Superset,
	          class = is_superset_of_t<set<Superset ...>, set<Features ...>>>
	file(file<own, Superset ...> &&) = delete;

	/**
	 * Copy assign from another file type instance.
	 *
	 * The origin file type instance must support all features
	 * supported by this file type.
	 *
	 * @tparam Ownership
	 *	The ownership status of the other file type
	 * @tparam Superset
	 *	The feature set of another file type
	 * @tparam Cond
	 *	Whether Superset is an actual superset of Features
	 * @param copy
	 *	The lvalue file to acquire the FILE object from
	 * @return
	 *	A self reference
	 */
	template <ownership Ownership, feature ... Superset,
	          class = is_superset_of_t<set<Superset ...>, set<Features ...>>>
	file & operator =(file<Ownership, Superset ...> const & copy) {
		this->handle = copy.get();
		return *this;
	}

	/**
	 * Must not move assign from files with ownership of their handle.
	 *
	 * @tparam Superset
	 *	The feature set of another FILE object owning file type
	 * @return
	 *	A self reference
	 */
	template <feature ... Superset,
	          class = is_superset_of_t<set<Superset ...>, set<Features ...>>>
	file & operator =(file<own, Superset ...> &&) = delete;

	/**
	 * Provide the internal FILE object pointer.
	 *
	 * Can be used to pass the file to legacy C interfaces.
	 *
	 * @return
	 *	A pointer to the managed FILE object
	 */
	FILE * get() const {
		return this->handle;
	}
};

/**
 * @defgroup stdio Standard I/O File Access
 *
 * A set of file instances providing access to stderr, stdout and
 * stdin.
 *
 * In theory these should be functions returning a reference to a
 * local static file object, to avoid global object initialisation
 * order issues.
 *
 * This would be annoying to access, though. In practice it works
 * the way it is and it would be hard to notice if it did not.
 *
 * @{
 */

/**
 * File access instances for stderr.
 */
inline file<link, write> ferr{stderr};

/**
 * File access instances for stdout.
 */
inline file<link, write> fout{stdout};

/**
 * File access instances for stdin.
 */
inline file<link, read> fin{stdin};

/** @} */

} /* namespace io */

} /* namespace sys */

#endif /* _POWERDXX_SYS_IO_HPP_ */

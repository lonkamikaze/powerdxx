/**
 * Implements generally useful functions not intended for inlining.
 *
 * @file
 */

#include "utility.hpp"

using namespace utility::literals;

utility::Underlined
utility::highlight(std::string const & str, ptrdiff_t const offs,
                   ptrdiff_t const len) {
	using std::cbegin;
	using std::cend;

	std::string txt;
	std::string ul;
	auto const beg = cbegin(str);
	auto const end = cend(str);
	int merge = 0;

	/* sanitise and underline the string character wise */
	for (auto it = begin(str); it != end; ++it) {
		/* underlining character ' ', '^', '~' or '\0' */
		char const ulc = (it - beg <  offs)        * ' ' +
		                 (it - beg == offs)        * '^' +
		                 (it - beg >  offs)        *
		                 (it - beg <  offs + len)  * '~';
		/* string style appends do not, unlike single character
		 * appends, inject 0 bytes into a string */
		char const ulcx2[]{ulc, ulc, 0};
		char const * const ulcx1 = ulcx2 + 1;
		/* utf-8 multi-byte heads */
		if (((*it & 0xe0) == 0xc0 && (merge = 1)) ||  /* 2-byte */
		    ((*it & 0xf0) == 0xe0 && (merge = 2)) ||  /* 3-byte */
		    ((*it & 0xf8) == 0xf0 && (merge = 3))) {  /* 4-byte */
			txt += *it;
			ul += ulcx1;
			continue;
		}
		/* utf-8 multi-byte tail, do not underline, because
		 * it is merged with a previous character */
		if (merge && (*it & 0xc0) == 0x80) {
			--merge;
			txt += *it;
			continue;
		}
		merge = 0;
		/* printf control characters */
		switch (*it) {
		case '\a':
			txt += "\\a";
			ul  += ulcx2;
			continue;
		case '\b':
			txt += "\\b";
			ul  += ulcx2;
			continue;
		case '\f':
			txt += "\\f";
			ul  += ulcx2;
			continue;
		case '\n':
			txt += "\\n";
			ul  += ulcx2;
			continue;
		case '\r':
			txt += "\\r";
			ul  += ulcx2;
			continue;
		case '\t':
			txt += "\\t";
			ul  += ulcx2;
			continue;
		case '\v':
			txt += "\\v";
			ul  += ulcx2;
			continue;
		case '\\':
			txt += "\\\\";
			ul  += ulcx2;
			continue;
		}
		/* other control characters and invalid code points */
		if (*it < ' ' || *it >= 0x7f) {
			auto const escaped = "\\%o"_fmt(*it & 0xff);
			for (auto const tch : escaped) {
				txt += tch;
				ul  += ulcx1;
			}
			continue;
		}
		/* regular characters */
		txt += *it;
		ul  += ulcx1;
	}
	if (offs >= end - beg) {
		ul += '^';
	}
	return {txt, ul};
}

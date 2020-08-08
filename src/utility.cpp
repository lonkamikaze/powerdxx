/**
 * Implements generally useful functions not intended for inlining.
 *
 * @file
 */

#include "utility.hpp"

using namespace utility::literals;

/**
 * File local scope.
 */
namespace {

/**
 * Append a string literal to a sanitised string.
 *
 * Updates the meta data along with the string.
 *
 * @tparam SizeV
 *	The string literal size, including the terminator
 * @param lhs
 *	The sanitised string to update
 * @param rhs
 *	The string literal to append
 */
template <std::size_t SizeV>
constexpr utility::Sanitised &
operator +=(utility::Sanitised & lhs, char const (& rhs)[SizeV]) {
	lhs.text += rhs;
	lhs.width += (SizeV - 1);
	return lhs;
}

} /* namespace */

utility::Sanitised  utility::sanitise(std::string_view const & str) {
	Sanitised result{};
	auto & text  = result.text;
	auto & width = result.width;
	auto const end = cend(str);
	for (auto it = cbegin(str); it != end; ++it) {
		/* utf-8 multi-byte */
		if (std::size_t bytes = 0;
		    ((*it & 0xe0) == 0xc0 && (bytes = 2)) ||  /* 2-byte */
		    ((*it & 0xf0) == 0xe0 && (bytes = 3)) ||  /* 3-byte */
		    ((*it & 0xf8) == 0xf0 && (bytes = 4))) {  /* 4-byte */
			text += *it;
			for (std::size_t i = 1; i < bytes; ++i) {
				if (it + 1 != end && (it[1] & 0xc0) == 0x80) {
					text += *(++it);
				}
			}
			++width;
			continue;
		}

		/* printf control characters */
		switch (*it) {
		case '\a':
			result += "\\a";
			continue;
		case '\b':
			result += "\\b";
			continue;
		case '\f':
			result += "\\f";
			continue;
		case '\n':
			result += "\\n";
			continue;
		case '\r':
			result += "\\r";
			continue;
		case '\t':
			result += "\\t";
			continue;
		case '\v':
			result += "\\v";
			continue;
		case '\\':
			result += "\\\\";
			continue;
		}
		/* other control characters and invalid code points */
		if (*it < ' ') {
			auto const escaped = "\\%o"_fmt(*it & 0xff);
			text += escaped;
			width += escaped.size();
			continue;
		}
		/* regular characters */
		text += *it;
		++width;
	}
	return result;
}

utility::Underlined
utility::highlight(std::string_view const & str, std::size_t const offs,
                   std::size_t const len) {
	/*
	 * Sanitise the string in 3 stages to get a separate count
	 * of visible characters for each stage.
	 */
	/* before the underlining offset */
	auto [text, width] = sanitise(str.substr(0, offs));
	Underlined result{std::move(text)};
	result.line.insert(0, width, ' ');

	/* the underlined section */
	result.line += '^';
	if (offs < str.size()) {
		auto [text, width] = sanitise(str.substr(offs, len));
		result.text += text;
		result.line.insert(result.line.size(), width - 1, '~');
	}

	/* behind the underlined section */
	if (offs + len < str.size()) {
		auto [text, width] = sanitise(str.substr(offs + len));
		result.text += text;
	}

	return result;
}

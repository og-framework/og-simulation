#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/TickDriverLog-rationale.md · docs/TickDriverLog-guards.md

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

enum class HostLogLevel : uint8_t
{
	Verbose,
	Log,
	Warning,
};

template <typename Allocator = std::allocator<char>>
using BasicHostLine = std::basic_string<char, std::char_traits<char>, Allocator>;

using HostLine = BasicHostLine<>;

#if defined(__GNUC__) || defined(__clang__)
#define OGSIM_HOST_LINE_FORMAT_CHECK __attribute__((format(printf, 2, 3)))
#else
#define OGSIM_HOST_LINE_FORMAT_CHECK
#endif

template <typename Allocator>
OGSIM_HOST_LINE_FORMAT_CHECK const char* formatHostLine(BasicHostLine<Allocator>& out, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	va_list retry;
	va_copy(retry, args);

	out.resize(out.capacity());
	const int length = std::vsnprintf(out.data(), out.size() + 1u, fmt, args);
	va_end(args);

	if (length < 0)
	{
		va_end(retry);
		out.clear();
		return out.c_str();
	}

	const size_t needed = static_cast<size_t>(length);
	if (needed <= out.size())
	{
		out.resize(needed);
		va_end(retry);
		return out.c_str();
	}

	out.resize(needed);
	// ⛔G-01  docs/TickDriverLog-guards.md
	std::vsnprintf(out.data(), needed + 1u, fmt, retry);
	va_end(retry);
	return out.c_str();
}

#undef OGSIM_HOST_LINE_FORMAT_CHECK

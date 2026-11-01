#pragma once

// Some original Steinberg VST2.4 SDK headers define VSTCALLBACK as __cdecl
// even on Linux. GCC/Clang on Linux use the platform's default C calling
// convention and do not understand MSVC's __cdecl keyword. Define only the
// spelling that the legacy header expects; do not modify or ship SDK files.
//
// This header must be included BEFORE <aeffectx.h>.
#if defined(__linux__) && (defined(__GNUC__) || defined(__clang__)) && !defined(__cdecl)
#define __cdecl
#endif

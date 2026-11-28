#pragma once
// SPDX-License-Identifier: MPL-2.0
#if defined(OG_STANDALONE_BUILD) && defined(OG_CHECKS_LIVE)
  // Standalone build with live checks, whatever NDEBUG says: a failed check prints and exits 3 at once.
  #include <cstdio>
  #include <cstdlib>
  #define OG_CHECK(cond, msg) \
    ((cond) ? static_cast<void>(0) \
            : (std::fprintf(stderr, "OG_CHECK failed: %s (%s:%d): %s\n", #cond, __FILE__, __LINE__, (msg)), \
               std::fflush(stderr), std::_Exit(3)))
#elif defined(OG_STANDALONE_BUILD)
  #include <cassert>
  #define OG_CHECK(cond, msg) assert((cond) && (msg))
#else
  // UE build: forward to checkf.
  // Note: TEXT() wraps a string literal, so the caller passes a plain string literal.
  #define OG_CHECK(cond, msg) checkf((cond), TEXT(msg))
#endif

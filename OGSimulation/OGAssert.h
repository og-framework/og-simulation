#pragma once
// SPDX-License-Identifier: MPL-2.0
#ifdef OG_STANDALONE_BUILD
  #include <cassert>
  #define OG_CHECK(cond, msg) assert((cond) && (msg))
#else
  // UE build: forward to checkf.
  // Note: TEXT() wraps a string literal, so the caller passes a plain string literal.
  #define OG_CHECK(cond, msg) checkf((cond), TEXT(msg))
#endif

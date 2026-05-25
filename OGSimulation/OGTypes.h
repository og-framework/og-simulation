#pragma once
// SPDX-License-Identifier: MPL-2.0
#ifdef OG_STANDALONE_BUILD
  #include <cstdint>
  using int32  = int32_t;
  using uint32 = uint32_t;
  // UE defines PI as a float constant — match it here.
  #ifndef PI
    #define PI 3.1415926535897932f
  #endif
#endif
// In UE builds, these typedefs and PI come from Core's PCH — no-op here.

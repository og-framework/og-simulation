#pragma once
// SPDX-License-Identifier: MPL-2.0
#ifdef OG_STANDALONE_BUILD
  #define OGSIMULATION_API
  #define OGBRAWLER_API
#endif
// In UE builds, these are defined by UBT's generated headers.

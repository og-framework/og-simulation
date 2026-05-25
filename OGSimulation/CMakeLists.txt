# SPDX-License-Identifier: MPL-2.0

file(GLOB_RECURSE OGSIM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

# UE-only files that cannot compile in the standalone CMake build:
#   OGSimulation.cpp — UE IMPLEMENT_MODULE stub
list(REMOVE_ITEM OGSIM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/OGSimulation.cpp"
)

add_library(og_simulation STATIC ${OGSIM_SOURCES})

target_include_directories(og_simulation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(og_simulation PUBLIC glm::glm-header-only)

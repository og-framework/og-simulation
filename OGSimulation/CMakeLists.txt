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

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(og_simulation PUBLIC /fp:precise)
elseif(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(og_simulation PUBLIC /fp:precise /clang:-ffp-contract=off)
else()
    target_compile_options(og_simulation PUBLIC -ffp-contract=off)
endif()

file(GLOB_RECURSE OGSIM_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

set(OGSIM_HEADER_TUS "")
foreach(header IN LISTS OGSIM_HEADERS)
    file(RELATIVE_PATH header_rel "${CMAKE_CURRENT_SOURCE_DIR}/.." "${header}")
    string(MAKE_C_IDENTIFIER "${header_rel}" header_id)
    set(header_tu "${CMAKE_CURRENT_BINARY_DIR}/header_check/${header_id}.cpp")
    set(header_tu_text "#include \"${header_rel}\"\n")
    if(EXISTS "${header_tu}")
        file(READ "${header_tu}" header_tu_old)
    else()
        set(header_tu_old "")
    endif()
    if(NOT header_tu_old STREQUAL header_tu_text)
        file(WRITE "${header_tu}" "${header_tu_text}")
    endif()
    list(APPEND OGSIM_HEADER_TUS "${header_tu}")
endforeach()

add_library(og_simulation_header_check OBJECT ${OGSIM_HEADER_TUS})
target_link_libraries(og_simulation_header_check PRIVATE og_simulation)
if(MSVC)
    target_compile_options(og_simulation_header_check PRIVATE /GR- /EHs-c- /wd4577)
    target_compile_definitions(og_simulation_header_check PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(og_simulation_header_check PRIVATE -fno-rtti -fno-exceptions)
endif()

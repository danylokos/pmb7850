cmake_minimum_required(VERSION 3.16)
project(cemu_carquet_deps LANGUAGES C)

set(CARQUET_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(CARQUET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CARQUET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CARQUET_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CARQUET_BUILD_INTEROP OFF CACHE BOOL "" FORCE)
set(CARQUET_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CARQUET_BUNDLE_DEPS ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_OpenMP TRUE CACHE BOOL "" FORCE)

# Carquet's Release profile uses LTO. Old GNU binutils need the compiler
# wrappers to load the matching LTO plugin when archiving static objects.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    find_program(CEMU_LTO_AR gcc-ar)
    find_program(CEMU_LTO_RANLIB gcc-ranlib)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    find_program(CEMU_LTO_AR llvm-ar)
    find_program(CEMU_LTO_RANLIB llvm-ranlib)
endif()
if(CEMU_LTO_AR)
    set(CMAKE_AR "${CEMU_LTO_AR}" CACHE FILEPATH "" FORCE)
endif()
if(CEMU_LTO_RANLIB)
    set(CMAKE_RANLIB "${CEMU_LTO_RANLIB}" CACHE FILEPATH "" FORCE)
endif()

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../vendor/carquet"
    "${CMAKE_CURRENT_BINARY_DIR}/carquet")

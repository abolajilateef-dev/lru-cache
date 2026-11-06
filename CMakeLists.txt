cmake_minimum_required(VERSION 3.14)
project(lru_cache CXX)

# --- Standard & flags -------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The spec asks for -O2 -g. We keep NDEBUG UNDEFINED on purpose so that the
# assert()s in check_invariants() (and the eviction-path asserts) stay live in
# every target. CMake's default Release config defines NDEBUG, which would
# silently delete those asserts; we therefore set our own flags and do not use
# CMAKE_BUILD_TYPE=Release.
if(NOT CMAKE_CXX_FLAGS)
  set(CMAKE_CXX_FLAGS "-O2 -g -Wall -Wextra")
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -Wall -Wextra")
endif()

# --- Sanitizer options ------------------------------------------------------
# Mutually exclusive: TSan cannot be combined with ASan/UBSan.
option(ENABLE_TSAN     "Build with ThreadSanitizer"            OFF)
option(ENABLE_ASAN_UBSAN "Build with Address+UndefinedSanitizer" OFF)

if(ENABLE_TSAN AND ENABLE_ASAN_UBSAN)
  message(FATAL_ERROR "ENABLE_TSAN and ENABLE_ASAN_UBSAN are mutually exclusive.")
endif()

set(SANITIZER_FLAGS "")
if(ENABLE_TSAN)
  set(SANITIZER_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
  message(STATUS "ThreadSanitizer enabled")
elseif(ENABLE_ASAN_UBSAN)
  set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
  message(STATUS "Address+UndefinedSanitizer enabled")
endif()

# --- Threads ---------------------------------------------------------------
find_package(Threads REQUIRED)

# --- GoogleTest via FetchContent -------------------------------------------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# --- Header-only interface library -----------------------------------------
add_library(lru_cache INTERFACE)
target_include_directories(lru_cache INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# --- Tests ------------------------------------------------------------------
add_executable(lru_tests tests/test_lru.cpp)
target_link_libraries(lru_tests PRIVATE lru_cache GTest::gtest GTest::gtest_main Threads::Threads)
if(SANITIZER_FLAGS)
  separate_arguments(SAN_LIST UNIX_COMMAND "${SANITIZER_FLAGS}")
  target_compile_options(lru_tests PRIVATE ${SAN_LIST})
  target_link_options(lru_tests PRIVATE ${SAN_LIST})
endif()

include(GoogleTest)
gtest_discover_tests(lru_tests)

# --- Benchmark --------------------------------------------------------------
add_executable(lru_bench bench/bench.cpp)
target_link_libraries(lru_bench PRIVATE lru_cache Threads::Threads)
if(SANITIZER_FLAGS)
  separate_arguments(SAN_LIST2 UNIX_COMMAND "${SANITIZER_FLAGS}")
  target_compile_options(lru_bench PRIVATE ${SAN_LIST2})
  target_link_options(lru_bench PRIVATE ${SAN_LIST2})
endif()

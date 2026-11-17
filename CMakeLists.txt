cmake_minimum_required(VERSION 3.14)
project(LimitOrderBook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build unit tests" ON)
option(LOB_BUILD_BENCHMARKS "Build benchmarks" ON)
option(LOB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Shared warning flags, applied through an interface target.
add_library(lob_warnings INTERFACE)
if(MSVC)
  target_compile_options(lob_warnings INTERFACE /W4 /permissive-)
  if(LOB_WARNINGS_AS_ERRORS)
    target_compile_options(lob_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(lob_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(LOB_WARNINGS_AS_ERRORS)
    target_compile_options(lob_warnings INTERFACE -Werror)
  endif()
endif()

# Core library: domain model + matching logic, no I/O. Source files are added
# here as later phases introduce them.
add_library(lob_core STATIC
  src/Order.cpp
  src/Trade.cpp
  src/PriceLevel.cpp
  src/OrderBook.cpp
  src/MatchingEngine.cpp
  src/OrderManager.cpp
)
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob_core PUBLIC cxx_std_17)
target_link_libraries(lob_core PRIVATE lob_warnings)

add_executable(lob_cli src/main.cpp)
target_link_libraries(lob_cli PRIVATE lob_core lob_warnings)

if(LOB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LOB_BUILD_BENCHMARKS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt)
  add_subdirectory(benchmarks)
endif()

# Benchmarks: reproducible performance measurements. Separate executables, deliberately
# not registered with CTest (they are measurements, not pass/fail tests) and refusing to
# run in unoptimized builds. See docs/BENCHMARKS.md.
if(CMAKE_BUILD_TYPE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _lob_bt)
  set(_lob_bench_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_lob_bt}}")
else()
  set(_lob_bench_flags "${CMAKE_CXX_FLAGS}")
endif()
string(STRIP "${_lob_bench_flags}" _lob_bench_flags)

set(_lob_bench_defs
  "LOB_BENCH_COMPILER=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\""
  "LOB_BENCH_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\""
  "LOB_BENCH_FLAGS=\"${_lob_bench_flags}\"")

# Workload generation and the RNG are shared with the correctness tests (tests/).
add_executable(lob_bench Benchmark.cpp)
target_include_directories(lob_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(lob_bench PRIVATE lob_core lob_warnings)
target_compile_definitions(lob_bench PRIVATE ${_lob_bench_defs})

add_executable(lob_alloc_profile AllocProfile.cpp)
target_include_directories(lob_alloc_profile PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(lob_alloc_profile PRIVATE lob_core lob_warnings)
target_compile_definitions(lob_alloc_profile PRIVATE ${_lob_bench_defs})
if(MINGW)  # route allocations made inside libstdc++ through the counting operator new
  target_link_options(lob_alloc_profile PRIVATE -static)
endif()

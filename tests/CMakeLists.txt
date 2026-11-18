# Each *Tests.cpp is its own executable and its own ctest entry, so a failure
# is attributed to a specific suite. The tiny framework in TestFramework.h
# avoids any external dependency (see docs/ARCHITECTURE.md).
set(LOB_TEST_SOURCES
  OrderTypesTests.cpp
  OrderTests.cpp
  TradeTests.cpp
  PriceLevelTests.cpp
  OrderBookTests.cpp
  MatchingEngineTests.cpp
  OrderManagerTests.cpp
  FaultInjectionTests.cpp
  CommandParserTests.cpp
  CliTests.cpp
  CliRecoveryTests.cpp
  SystemInvariantTests.cpp
  ReferenceModelTests.cpp
  RandomizedWorkloadTests.cpp
  BoundaryTests.cpp
  AdversarialTests.cpp
  SystemFaultTests.cpp
)

foreach(src ${LOB_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE lob_ui lob_core lob_warnings)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

# SystemFaultTests counts every allocation to detect leaks and double frees. With the
# shared libstdc++ DLL, std::string (an extern template) allocates inside the DLL through
# the DLL's own operator new, bypassing the replacement in this executable, so the counts
# would be skewed. Linking statically makes ALL allocations go through the replacement.
if(MINGW)  # on ELF platforms symbol interposition already routes the library's allocations here
  target_link_options(SystemFaultTests PRIVATE -static)
endif()

# CliEndToEndTests runs the real lob_cli executable, so it is added by hand: it needs
# the executable built first and receives its path as an argument.
add_executable(CliEndToEndTests CliEndToEndTests.cpp)
target_link_libraries(CliEndToEndTests PRIVATE lob_warnings)
add_dependencies(CliEndToEndTests lob_cli)
add_test(NAME CliEndToEndTests COMMAND CliEndToEndTests $<TARGET_FILE:lob_cli>)

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
)

foreach(src ${LOB_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE lob_core lob_warnings)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

cmake_minimum_required(VERSION 3.20)
project(deterministic_exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EXSIM_BUILD_TESTS "Build tests" ON)
option(EXSIM_BUILD_BENCH "Build benchmarks" ON)

add_library(exsim
  src/order_entry.cpp
  src/market_data.cpp
  src/order_book.cpp
  src/risk.cpp
  src/journal.cpp
  src/exchange.cpp
  src/subscriber.cpp
)

target_include_directories(exsim PUBLIC include)
function(exsim_target_warnings target)
  if (MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endfunction()

exsim_target_warnings(exsim)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_definitions(exsim PUBLIC NDEBUG)
endif()

if (EXSIM_BUILD_TESTS)
  enable_testing()
  add_executable(exsim_tests tests/exsim_tests.cpp)
  target_link_libraries(exsim_tests PRIVATE exsim)
  exsim_target_warnings(exsim_tests)
  add_test(NAME exsim_tests COMMAND exsim_tests)
endif()

if (EXSIM_BUILD_BENCH)
  add_executable(bench_exchange_replay bench/bench_exchange_replay.cpp)
  target_link_libraries(bench_exchange_replay PRIVATE exsim)
  exsim_target_warnings(bench_exchange_replay)

  add_executable(replay_check tools/replay_check.cpp)
  target_link_libraries(replay_check PRIVATE exsim)
  exsim_target_warnings(replay_check)
endif()

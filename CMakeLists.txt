cmake_minimum_required(VERSION 3.20)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_BUILD_TESTS "Build the test suite" ON)
option(LOB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LOB_TSAN "Build with ThreadSanitizer" OFF)
option(LOB_NATIVE "Tune for the build machine's CPU (-march=native)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(LOB_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
if(LOB_NATIVE)
  add_compile_options(-march=native)
endif()

find_package(Threads REQUIRED)

add_library(lob src/order_book.cc src/engine.cc)
target_include_directories(lob PUBLIC include)
target_link_libraries(lob PUBLIC Threads::Threads)

if(EMSCRIPTEN)
  # Browser demo: only the single-threaded book is compiled to WebAssembly.
  add_executable(lob_wasm web/bindings.cc src/order_book.cc)
  target_include_directories(lob_wasm PRIVATE include web)
  target_link_options(lob_wasm PRIVATE -lembind -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createOrderBook
    -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web)
  set_target_properties(lob_wasm PROPERTIES OUTPUT_NAME orderbook SUFFIX ".mjs")
  return()
endif()

add_executable(lob_bench bench/lob_bench.cc)
target_link_libraries(lob_bench PRIVATE lob)

add_executable(lob_replay tools/lob_replay.cc)
target_link_libraries(lob_replay PRIVATE lob)

if(LOB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests
    tests/order_book_test.cc
    tests/reference_test.cc
    tests/primitives_test.cc
    tests/engine_test.cc
    tests/web_session_test.cc
  )
  target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)
endif()

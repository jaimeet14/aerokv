cmake_minimum_required(VERSION 3.16)
project(aerokv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(AEROKV_BUILD_TESTS "Build unit tests" ON)
option(AEROKV_BUILD_BENCH "Build db_bench (needs SQLite3 for the comparison)" ON)
set(AEROKV_SANITIZE "" CACHE STRING "Sanitizer: address, thread or empty")

if(AEROKV_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(AEROKV_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

add_library(aerokv
  src/bloom.cc
  src/crc32c.cc
  src/db.cc
  src/file_util.cc
  src/iterator.cc
  src/memtable.cc
  src/sstable.cc
  src/wal.cc)
target_include_directories(aerokv PUBLIC include PRIVATE src)
target_compile_options(aerokv PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(aerokv PUBLIC Threads::Threads)

if(AEROKV_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(aerokv_tests tests/unit_test.cc tests/db_test.cc)
  target_include_directories(aerokv_tests PRIVATE src)
  target_link_libraries(aerokv_tests PRIVATE aerokv GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(aerokv_tests DISCOVERY_TIMEOUT 60)
endif()

if(AEROKV_BUILD_BENCH)
  find_package(SQLite3)
  add_executable(db_bench bench/db_bench.cc)
  target_link_libraries(db_bench PRIVATE aerokv)
  if(SQLite3_FOUND)
    target_compile_definitions(db_bench PRIVATE AEROKV_WITH_SQLITE)
    target_link_libraries(db_bench PRIVATE SQLite::SQLite3)
  endif()
endif()

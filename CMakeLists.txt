cmake_minimum_required(VERSION 3.16)
project(cpp-url-shortener VERSION 1.0.0 LANGUAGES CXX)

# -------------------------------------------------------
# C++17 required throughout the project
# -------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -------------------------------------------------------
# Default build type when none is specified
# -------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# -------------------------------------------------------
# Place compiled binaries in build/bin/
# -------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# -------------------------------------------------------
# Compiler diagnostics
# -------------------------------------------------------
add_compile_options(
  -Wall
  -Wextra
  -Wpedantic
  -Wno-unused-parameter   # suppressed: httplib generates several of these
)

# -------------------------------------------------------
# System dependencies
# Install on Ubuntu: sudo apt install libsqlite3-dev
# -------------------------------------------------------
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

# -------------------------------------------------------
# Third-party single-header libraries
# Run  bash scripts/setup.sh  BEFORE cmake to download these.
# -------------------------------------------------------
set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")

if(NOT EXISTS "${THIRD_PARTY_DIR}/httplib.h")
  message(FATAL_ERROR
    "\n"
    "  [MISSING] third_party/httplib.h not found!\n"
    "  Please run:  bash scripts/setup.sh\n"
    "  Then re-run cmake.\n")
endif()

if(NOT EXISTS "${THIRD_PARTY_DIR}/nlohmann/json.hpp")
  message(FATAL_ERROR
    "\n"
    "  [MISSING] third_party/nlohmann/json.hpp not found!\n"
    "  Please run:  bash scripts/setup.sh\n"
    "  Then re-run cmake.\n")
endif()

# -------------------------------------------------------
# Shared static library
# Linked by both the main executable and the test binary.
# -------------------------------------------------------
add_library(url_shortener_lib STATIC
  src/database.cpp
  src/url_service.cpp
  src/http_server.cpp
)

target_include_directories(url_shortener_lib PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${THIRD_PARTY_DIR}            # exposes httplib.h and nlohmann/json.hpp
)

target_link_libraries(url_shortener_lib PUBLIC
  sqlite3
  Threads::Threads
)

# -------------------------------------------------------
# Main server executable
# -------------------------------------------------------
add_executable(cpp-url-shortener src/main.cpp)
target_link_libraries(cpp-url-shortener PRIVATE url_shortener_lib)

# -------------------------------------------------------
# Test executable  (ctest integration)
# -------------------------------------------------------
enable_testing()

add_executable(test_url_service tests/test_url_service.cpp)
target_link_libraries(test_url_service PRIVATE url_shortener_lib)

add_test(
  NAME    URLServiceTests
  COMMAND ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test_url_service
)

# -------------------------------------------------------
# Summary
# -------------------------------------------------------
message(STATUS "")
message(STATUS "  cpp-url-shortener ready:")
message(STATUS "    SQLite3 version  : ${SQLite3_VERSION}")
message(STATUS "    Binaries will be : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/")
message(STATUS "")


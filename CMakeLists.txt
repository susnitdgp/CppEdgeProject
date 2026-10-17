cmake_minimum_required(VERSION 3.25)
project(CppEdge VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Some fetched projects still declare an old cmake_minimum_required; CMake 4 needs this to accept them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)

# ---- system packages (apt: libcurl4-openssl-dev libhiredis-dev libsimdjson-dev libssl-dev)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_library(HIREDIS_LIB hiredis REQUIRED)     # redis++_static does not carry it to the final link
find_package(simdjson CONFIG QUIET)
if(NOT simdjson_FOUND)
  FetchContent_Declare(simdjson GIT_REPOSITORY https://github.com/simdjson/simdjson.git
                                GIT_TAG v3.13.0 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(simdjson)
endif()

# ---- fetched
FetchContent_Declare(quill GIT_REPOSITORY https://github.com/odygrd/quill.git
                           GIT_TAG v10.0.1 GIT_SHALLOW TRUE)
FetchContent_Declare(concurrentqueue GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
                                     GIT_TAG v1.0.4 GIT_SHALLOW TRUE)
set(REDIS_PLUS_PLUS_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(REDIS_PLUS_PLUS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(REDIS_PLUS_PLUS_CXX_STANDARD 17 CACHE STRING "" FORCE)
FetchContent_Declare(redis_plus_plus GIT_REPOSITORY https://github.com/sewenew/redis-plus-plus.git
                                     GIT_TAG 1.3.15 GIT_SHALLOW TRUE)
set(USE_TLS ON CACHE BOOL "" FORCE)            # IXWebSocket: wss:// through OpenSSL
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)          # per-message deflate is disabled anyway
FetchContent_Declare(ixwebsocket GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
                                 GIT_TAG v11.4.6 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(quill concurrentqueue redis_plus_plus ixwebsocket)

# ---- bot
add_executable(cpp_edge src/main.cpp)
target_include_directories(cpp_edge PRIVATE include ${concurrentqueue_SOURCE_DIR})
target_compile_definitions(cpp_edge PRIVATE EDGE_VERSION="${PROJECT_VERSION}")
target_compile_options(cpp_edge PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3 -march=native>)
target_link_libraries(cpp_edge PRIVATE
  ixwebsocket OpenSSL::SSL OpenSSL::Crypto CURL::libcurl
  simdjson::simdjson quill::quill redis++_static ${HIREDIS_LIB} Threads::Threads)

# ---- tools
add_executable(rest_probe tools/rest_probe.cpp)
target_include_directories(rest_probe PRIVATE include ${concurrentqueue_SOURCE_DIR})
target_compile_options(rest_probe PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rest_probe PRIVATE CURL::libcurl simdjson::simdjson redis++_static ${HIREDIS_LIB} Threads::Threads)

add_executable(backtest tools/backtest.cpp)
target_include_directories(backtest PRIVATE include ${concurrentqueue_SOURCE_DIR})
target_compile_options(backtest PRIVATE -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O3 -march=native>)
target_link_libraries(backtest PRIVATE CURL::libcurl simdjson::simdjson redis++_static ${HIREDIS_LIB} Threads::Threads)

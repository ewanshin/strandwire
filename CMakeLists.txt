# 3.21 = CMakePresets.json version 3. Ubuntu 22.04 LTS ships 3.22, so nothing newer is used.
cmake_minimum_required(VERSION 3.21)
project(strandwire LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include/asio.hpp)
  message(FATAL_ERROR "asio submodule is missing. Run: git submodule update --init")
endif()

find_package(Threads REQUIRED)

add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(asio INTERFACE Threads::Threads)
if(WIN32)
  target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(asio INTERFACE ws2_32 mswsock)
endif()

# protobuf 3.21.12 (last series without the Abseil dependency), full runtime, built from the
# submodule. The hand-maintained Visual Studio solution does not use this; it links the copy
# installed by tools/build_protobuf.ps1.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/protobuf/CMakeLists.txt)
  message(FATAL_ERROR "protobuf submodule is missing. Run: git submodule update --init")
endif()
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/protobuf EXCLUDE_FROM_ALL)
# Treat protobuf's headers as system headers for everything that links it, so their warnings
# stay out of our /W4 and -Wall -Wextra builds. (add_subdirectory(... SYSTEM) would do this but
# needs CMake 3.25.)
get_target_property(_protobuf_includes libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(libprotobuf PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_protobuf_includes}")

add_library(netsys_options INTERFACE)
if(MSVC)
  target_compile_options(netsys_options INTERFACE /W4 /utf-8 /EHsc)
else()
  target_compile_options(netsys_options INTERFACE -Wall -Wextra)
endif()

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include/spdlog/spdlog.h)
  message(FATAL_ERROR "spdlog submodule is missing. Run: git submodule update --init")
endif()

add_subdirectory(common)
add_subdirectory(LogLib)
add_subdirectory(ProtoLib)
add_subdirectory(ServerLib)
add_subdirectory(NetworkServer)
add_subdirectory(NetworkClient)
add_subdirectory(DummyClient)

enable_testing()
add_subdirectory(tests)

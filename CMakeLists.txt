cmake_minimum_required(VERSION 3.24)
project(fsnet-godot VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

# This is the complete fsnet build boundary: an installed binary SDK only.
find_package(fsnet CONFIG REQUIRED COMPONENTS client)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
include(FetchContent)
set(FSNET_GODOT_CPP_SOURCE_DIR "" CACHE PATH "Existing pinned godot-cpp source (optional)")
set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/build_profile.json" CACHE FILEPATH "" FORCE)
set(GODOTCPP_TARGET template_release CACHE STRING "" FORCE)
set(GODOTCPP_DISABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(GODOTCPP_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
if(FSNET_GODOT_CPP_SOURCE_DIR)
  add_subdirectory("${FSNET_GODOT_CPP_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp")
else()
  FetchContent_Declare(godot-cpp
    URL https://codeload.github.com/godotengine/godot-cpp/tar.gz/e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77
    URL_HASH SHA256=d4c03daf46c8bef4544614182b0762c3e32789151c7f8f44e8eb2053d5cddeba
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(godot-cpp)
endif()
get_target_property(fsnet_godot_cpp_includes godot-cpp INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET godot-cpp PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${fsnet_godot_cpp_includes}")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" fsnet_cpu)
if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES MATCHES ";")
  set(fsnet_cpu "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(fsnet_cpu MATCHES "^(amd64|x86_64|x64)$")
  set(fsnet_arch x86_64)
elseif(fsnet_cpu MATCHES "^(arm64|aarch64)$")
  set(fsnet_arch arm64)
else()
  message(FATAL_ERROR "Unsupported CPU: ${fsnet_cpu}")
endif()
if(WIN32)
  set(fsnet_os windows)
elseif(APPLE)
  set(fsnet_os macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(fsnet_os linux)
else()
  message(FATAL_ERROR "Only Linux, Windows and macOS desktop builds are supported")
endif()
set(FSNET_GODOT_PLATFORM "${fsnet_os}-${fsnet_arch}" CACHE INTERNAL "Release platform")
set(FSNET_GODOT_BINARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/addons/fsnet/bin" CACHE PATH "Native addon output")
file(MAKE_DIRECTORY "${FSNET_GODOT_BINARY_DIR}")
add_library(fsnet-godot SHARED
  src/fsnet_client.cpp src/fsnet_map.cpp src/value_codec.cpp src/register_types.cpp)
target_link_libraries(fsnet-godot PRIVATE godot::cpp fsnet::client)
target_compile_options(fsnet-godot PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter>)
set_target_properties(fsnet-godot PROPERTIES
  OUTPUT_NAME "fsnet_godot.${fsnet_os}.${fsnet_arch}"
  LIBRARY_OUTPUT_DIRECTORY "${FSNET_GODOT_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${FSNET_GODOT_BINARY_DIR}"
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(fsnet-godot PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${config} "${FSNET_GODOT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_${config} "${FSNET_GODOT_BINARY_DIR}")
endforeach()
if(WIN32)
  set_target_properties(fsnet-godot PROPERTIES PREFIX "")
elseif(APPLE)
  set_target_properties(fsnet-godot PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
else()
  set_target_properties(fsnet-godot PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
add_custom_command(TARGET fsnet-godot POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fsnet::core> "${FSNET_GODOT_BINARY_DIR}/")

option(FSNET_GODOT_BUILD_TEST_SERVER "Build the standalone public-SDK integration fixture" ON)
if(FSNET_GODOT_BUILD_TEST_SERVER)
  find_package(Threads REQUIRED)
  add_executable(fsnet-godot-test-server tests/native_server.cpp)
  target_link_libraries(fsnet-godot-test-server PRIVATE fsnet::client Threads::Threads)
  if(WIN32)
    add_custom_command(TARGET fsnet-godot-test-server POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:fsnet::core>
        "$<TARGET_FILE_DIR:fsnet-godot-test-server>/")
  endif()
endif()

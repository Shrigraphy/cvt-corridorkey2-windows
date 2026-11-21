# CorridorKey 2 for Resolve - build
# From Creative Video Tips (CVT)  |  https://creativevideotips.com
# Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
cmake_minimum_required(VERSION 3.20)
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES arm64)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
endif()
project(CorridorKey2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Source paths compiled into the binary (asserts, error messages) say "~"
# instead of the builder's home folder, so no user name ships.
if(MSVC)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(/utf-8)
else()
  add_compile_options("-ffile-prefix-map=$ENV{HOME}=~")
endif()

set(OFX ${CMAKE_SOURCE_DIR}/third_party/openfx)
file(GLOB OFX_SUPPORT ${OFX}/Support/Library/*.cpp)

# OpenEXR + Imath, built static by third_party/build_openexr.sh (never Homebrew's).
set(CK2_DEPS "${CMAKE_SOURCE_DIR}/build/deps/install" CACHE PATH "Pinned OpenEXR and Imath installation")
find_package(Imath CONFIG REQUIRED PATHS ${CK2_DEPS} NO_DEFAULT_PATH)
find_package(OpenEXR CONFIG REQUIRED PATHS ${CK2_DEPS} NO_DEFAULT_PATH)

add_library(ck2core STATIC src/ck2_exr.cpp src/ck2_job.cpp)
target_include_directories(ck2core PUBLIC src)
target_link_libraries(ck2core PUBLIC OpenEXR::OpenEXR)
if(MSVC)
  target_compile_options(ck2core PRIVATE /W4)
else()
  target_compile_options(ck2core PRIVATE -Wall -Wextra)
endif()

add_library(CorridorKey2 MODULE src/ck2_plugin.cpp ${OFX_SUPPORT})
target_include_directories(CorridorKey2 PRIVATE ${OFX}/include ${OFX}/Support/include
                           ${OFX}/Support/Library)
target_link_libraries(CorridorKey2 PRIVATE ck2core)
set_target_properties(CorridorKey2 PROPERTIES PREFIX "" SUFFIX ".ofx"
                      CXX_VISIBILITY_PRESET hidden)
# Only the OFX entry points are exported.
if(APPLE)
  target_link_options(CorridorKey2 PRIVATE
    -Wl,-exported_symbol,_OfxGetNumberOfPlugins -Wl,-exported_symbol,_OfxGetPlugin)
elseif(WIN32)
  target_link_libraries(CorridorKey2 PRIVATE shell32)
endif()

add_executable(ck2_tests tests/test_core.cpp)
target_link_libraries(ck2_tests PRIVATE ck2core)

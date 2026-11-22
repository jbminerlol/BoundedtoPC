cmake_minimum_required(VERSION 3.20)
project(BoundedToThePersonalComputer C)

set(CMAKE_C_STANDARD 11)
find_package(SDL3 REQUIRED CONFIG)

file(GLOB EB_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
file(GLOB EB_GEN CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/build/gen/*.c)

add_executable(EarthBound WIN32 ${EB_SRC} ${EB_GEN})
target_include_directories(EarthBound PRIVATE src build/gen)
# Media Foundation (part of Windows) encodes the F9 recordings
target_link_libraries(EarthBound PRIVATE SDL3::SDL3 mfplat mfreadwrite mfuuid ole32)
target_compile_options(EarthBound PRIVATE -Wall -Wno-unused-label -Wno-unused-variable
                       -Wno-unused-function -fno-strict-aliasing)
# the generated code is huge and branchy: -O1 keeps compile times sane
set_source_files_properties(${EB_GEN} PROPERTIES COMPILE_OPTIONS "-O1;-w")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_link_options(EarthBound PRIVATE -s)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/res/earthbound.rc)
  target_sources(EarthBound PRIVATE res/earthbound.rc)
endif()

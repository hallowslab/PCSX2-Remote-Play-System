cmake_minimum_required(VERSION 3.16)

project(PCSX2RemotePlay)

set(CMAKE_CXX_STANDARD 17)

# Platform-specific flags
if(WIN32)
    add_definitions(-DPLATFORM_WINDOWS)
elseif(UNIX)
    add_definitions(-DPLATFORM_LINUX)
endif()

# Dependencies
find_package(PkgConfig REQUIRED)
find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)

find_library(AVCODEC_LIB avcodec REQUIRED)
find_library(AVFORMAT_LIB avformat REQUIRED)
find_library(SWSCALE_LIB swscale REQUIRED)
find_library(AVUTIL_LIB avutil REQUIRED)

# Add your executable here FIRST
add_executable(remote-play
    src/main.cpp
    src/host/host.cpp
    src/client/client.cpp
)

# Now you can link to it
target_link_libraries(remote-play
    PRIVATE
        SDL3::SDL3
        ${AVCODEC_LIB}
        ${AVFORMAT_LIB}
        ${SWSCALE_LIB}
        ${AVUTIL_LIB}
)
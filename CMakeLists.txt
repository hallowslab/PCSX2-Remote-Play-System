cmake_minimum_required(VERSION 3.16)

project(PCSX2RemotePlay)

set(CMAKE_CXX_STANDARD 17)

# Platform-specific flags
if(WIN32)
    add_definitions(-DPLATFORM_WINDOWS)
elseif(UNIX)
    add_definitions(-DPLATFORM_LINUX)
endif()

# Common definitions
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
endif()

# Dependencies
find_package(PkgConfig REQUIRED)
find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3)

find_library(AVCODEC_LIB avcodec REQUIRED)
find_library(AVFORMAT_LIB avformat REQUIRED)
find_library(SWSCALE_LIB swscale REQUIRED)
find_library(SWRESAMPLE_LIB swresample REQUIRED)
find_library(AVUTIL_LIB avutil REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)


# Add your executable here FIRST
add_executable(remote-play
    src/main.cpp
    src/host/host.cpp
    src/host/encoder/encoder.cpp
    src/host/capture/dxgi_capture.cpp
    src/host/capture/audio_capture.cpp
    src/host/input/sendinput_injector.cpp
    src/host/input/input_mapper.cpp
    src/host/input/hidmaestro_injector.cpp
    src/host/process/game_config.cpp
    src/host/process/process_manager.cpp
    src/shared/protocol/protocol.cpp
    src/client/client.cpp
)

# Now you can link to it
target_link_libraries(remote-play
    PRIVATE
        SDL3::SDL3
        ${AVCODEC_LIB}
        ${AVFORMAT_LIB}
        ${SWSCALE_LIB}
        ${SWRESAMPLE_LIB}
        ${AVUTIL_LIB}
)

if(WIN32)
    target_link_libraries(remote-play PRIVATE ws2_32 d3d11 dxgi winmm shell32)
endif()

# Link ENet
if(WIN32)
    find_package(unofficial-enet CONFIG REQUIRED)
    target_link_libraries(remote-play PRIVATE unofficial::enet::enet)
else()
    find_package(enet REQUIRED)
    target_link_libraries(remote-play PRIVATE enet)
endif()

# Copy a sample config next to the binary so the user has a template to edit.
add_custom_command(TARGET remote-play POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config.ini.example
        $<TARGET_FILE_DIR:remote-play>/config.ini.example
    COMMENT "Copying sample config.ini.example")

# Build + ship the HIDMaestro analog-controller bridge (Windows, optional).
if(WIN32)
    set(BRIDGE_DIR "${CMAKE_SOURCE_DIR}/hidmaestro-bridge")
    if(EXISTS "${BRIDGE_DIR}/HIDMaestro.Core.dll")
        find_program(DOTNET_EXE dotnet)
        if(DOTNET_EXE)
            add_custom_command(TARGET remote-play POST_BUILD
                COMMAND ${DOTNET_EXE} publish "${BRIDGE_DIR}/HidMaestroBridge.csproj"
                        -c Release -r win-x64 -o "$<TARGET_FILE_DIR:remote-play>"
                        --nologo -v quiet
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${BRIDGE_DIR}/HIDMAESTRO_LICENSE"
                        "$<TARGET_FILE_DIR:remote-play>/HIDMAESTRO_LICENSE"
                COMMENT "Publishing HIDMaestro bridge + license")
        else()
            message(WARNING "dotnet not found; HIDMaestro bridge will not be built")
        endif()
    else()
        message(WARNING "HIDMaestro.Core.dll missing from hidmaestro-bridge/; analog input unavailable")
    endif()
endif()

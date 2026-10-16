cmake_minimum_required(VERSION 3.16)
project(wiimosc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# wiiuse, built as a static library without its examples/tests
set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLE_SDL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(external/wiiuse EXCLUDE_FROM_ALL)
# Without WIIUSE_STATIC the header declares dllimport, which breaks static linking on MSVC.
target_compile_definitions(wiiuse PUBLIC WIIUSE_STATIC)
target_include_directories(wiiuse PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/wiiuse/src)

add_library(pairing STATIC src/pairing/WiimotePairer.h)
target_include_directories(pairing PUBLIC src)
if(WIN32)
    target_sources(pairing PRIVATE src/pairing/WiimotePairer_win.cpp)
    target_link_libraries(pairing PRIVATE bthprops ws2_32)
else()
    message(FATAL_ERROR "Pairing is only implemented for Windows so far")
endif()

add_library(hub STATIC src/hub/WiimoteHub.h src/hub/WiimoteHub.cpp)
target_link_libraries(hub PUBLIC pairing wiiuse)
if(WIN32)
    target_link_libraries(hub PRIVATE hid) # HidD_GetSerialNumberString, for Bluetooth addresses
endif()

add_library(osc STATIC src/osc/OscSender.h src/osc/OscSender.cpp)
target_include_directories(osc PUBLIC src)
if(WIN32)
    target_link_libraries(osc PRIVATE ws2_32)
endif()

add_executable(wiimosc-pair src/pairtool/main.cpp)
target_link_libraries(wiimosc-pair PRIVATE hub)

add_executable(wiimosc-stream
    src/stream/main.cpp
    src/stream/WiimoteStreamer.h
    src/stream/WiimoteStreamer.cpp)
target_link_libraries(wiimosc-stream PRIVATE hub osc)

add_executable(wiimosc-speaker
    src/speaker/main.cpp
    src/speaker/Audio.h
    src/speaker/Audio.cpp
    src/speaker/WiimoteSpeaker.h
    src/speaker/WiimoteSpeaker.cpp)
target_link_libraries(wiimosc-speaker PRIVATE hub)
if(WIN32)
    target_sources(wiimosc-speaker PRIVATE src/speaker/Audio_win.cpp)
    target_compile_definitions(wiimosc-speaker PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # Media Foundation (decoding), winmm (timer resolution), hid (HidD_SetOutputReport)
    target_link_libraries(wiimosc-speaker PRIVATE mfplat mfreadwrite mfuuid ole32 winmm hid)
endif()

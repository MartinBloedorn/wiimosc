cmake_minimum_required(VERSION 3.16)
project(wiimosc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# wiiuse, built as a static library without its examples/tests
set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLE_SDL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# Full accelerometer precision (Wiimote 10/9/9 bits, Nunchuk 10 bits) instead of 8 bits per axis. Only changes the
# values of accel/gforce/orient, not their units or wiiuse's struct layout, so wiimosc needn't define the macro.
set(WITH_ACCEL_10BIT ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(external/wiiuse EXCLUDE_FROM_ALL)
# Without WIIUSE_STATIC the header declares dllimport, which breaks static linking on MSVC.
target_compile_definitions(wiiuse PUBLIC WIIUSE_STATIC)
target_include_directories(wiiuse PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/wiiuse/src)

# xioTechnologies Fusion (Madgwick's AHRS), C library
add_subdirectory(external/Fusion/Fusion ${CMAKE_BINARY_DIR}/external/Fusion EXCLUDE_FROM_ALL)

add_library(motionfusion STATIC
    src/fusion/Orientation.h
    src/fusion/Orientation.cpp
    src/fusion/MotionPlusCalibration.h
    src/fusion/MotionPlusCalibration.cpp
    src/fusion/AbstractOrientationFilter.h
    src/fusion/MadgwickFilter.h
    src/fusion/MadgwickFilter.cpp
    src/fusion/IrAnchors.h
    src/fusion/IrAnchors.cpp
    src/fusion/MotionFusion.h
    src/fusion/MotionFusion.cpp)
target_include_directories(motionfusion PUBLIC src)
target_link_libraries(motionfusion PRIVATE Fusion)
# Fusion's headers use C99 designated initializers, which C++ only has since C++20. Only this target's sources
# include them; its own headers stay C++17.
set_target_properties(motionfusion PROPERTIES CXX_STANDARD 20)

add_library(pairing STATIC src/pairing/WiimotePairer.h)
target_include_directories(pairing PUBLIC src)
if(WIN32)
    target_sources(pairing PRIVATE src/pairing/WiimotePairer_win.cpp)
    target_link_libraries(pairing PRIVATE bthprops ws2_32)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(pairing PRIVATE src/pairing/WiimotePairer_mac.mm)
    target_compile_options(pairing PRIVATE -fobjc-arc)
    target_link_libraries(pairing PRIVATE "-framework IOBluetooth" "-framework Foundation")
    # Every tool that pairs accesses Bluetooth, which macOS only allows with a usage description in the
    # executable's Info.plist; command-line tools embed it in a section.
    target_link_options(pairing INTERFACE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/src/pairing/Info.plist")
else()
    # bluetoothd pairs the remotes (D-Bus through GIO); wiiuse then opens them as hidraw devices.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
    target_sources(pairing PRIVATE src/pairing/WiimotePairer_linux.cpp)
    target_link_libraries(pairing PRIVATE PkgConfig::GIO)
endif()

add_library(hub STATIC src/hub/WiimoteHub.h src/hub/WiimoteHub.cpp)
target_link_libraries(hub PUBLIC pairing wiiuse)
if(WIN32)
    target_link_libraries(hub PRIVATE hid) # HidD_GetSerialNumberString, for Bluetooth addresses
endif()

add_library(osc STATIC src/osc/OscSender.h src/osc/OscSender.cpp src/osc/OscReceiver.h src/osc/OscReceiver.cpp)
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
target_link_libraries(wiimosc-stream PRIVATE hub osc motionfusion speaker)

# Recording and replaying raw motion data, for tuning the sensor fusion offline (data/recordings/README.md)
add_executable(wiimosc-record src/record/main.cpp)
target_link_libraries(wiimosc-record PRIVATE hub motionfusion)

add_executable(wiimosc-replay src/replay/main.cpp)
target_link_libraries(wiimosc-replay PRIVATE motionfusion)

# Speaker streaming and audio decoding, shared by wiimosc-speaker and wiimosc-stream (mote/speaker/...)
add_library(speaker STATIC
    src/speaker/Audio.h
    src/speaker/Audio.cpp
    src/speaker/SoundSources.h
    src/speaker/SoundSources.cpp
    src/speaker/WiimoteSpeaker.h
    src/speaker/WiimoteSpeaker.cpp)
target_include_directories(speaker PUBLIC src)
target_link_libraries(speaker PRIVATE hub)
if(WIN32)
    target_sources(speaker PRIVATE src/speaker/Audio_win.cpp)
    target_compile_definitions(speaker PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # Media Foundation (decoding), winmm (timer resolution), hid (HidD_SetOutputReport)
    target_link_libraries(speaker PRIVATE mfplat mfreadwrite mfuuid ole32 winmm hid)
elseif(APPLE)
    target_sources(speaker PRIVATE src/speaker/Audio_mac.cpp)
    target_link_libraries(speaker PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
    target_sources(speaker PRIVATE src/speaker/Audio_linux.cpp)
    target_link_libraries(speaker PRIVATE PkgConfig::SNDFILE)
endif()

add_executable(wiimosc-speaker src/speaker/main.cpp)
target_link_libraries(wiimosc-speaker PRIVATE hub speaker)
if(WIN32)
    target_compile_definitions(wiimosc-speaker PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

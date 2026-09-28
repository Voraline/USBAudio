cmake_minimum_required(VERSION 3.24)
project(UsbAudioSender LANGUAGES C CXX)

include(FetchContent)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_FLOAT_API OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
)

FetchContent_MakeAvailable(opus)

add_executable(UsbAudioSender
    windows/src/Main.cpp
    windows/src/AudioCapture.cpp
    windows/src/UsbAccessory.cpp
)

target_compile_features(UsbAudioSender PRIVATE cxx_std_20)
target_include_directories(UsbAudioSender PRIVATE shared windows/src)
target_link_libraries(UsbAudioSender PRIVATE opus ole32 uuid avrt setupapi winusb advapi32)
target_compile_definitions(UsbAudioSender PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(UsbAudioSender PRIVATE /W4 /O2 /EHsc)
endif()

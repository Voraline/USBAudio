cmake_minimum_required(VERSION 3.22.1)
project(UsbAudioReceiver LANGUAGES C CXX)

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

add_library(usb_audio_native SHARED Receiver.cpp)
target_compile_features(usb_audio_native PRIVATE cxx_std_20)
target_include_directories(usb_audio_native PRIVATE ../../../../../shared)
target_link_libraries(usb_audio_native PRIVATE opus aaudio log android)
target_compile_options(usb_audio_native PRIVATE -O2 -Wall -Wextra -Werror=return-type)

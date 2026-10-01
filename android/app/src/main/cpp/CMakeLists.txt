cmake_minimum_required(VERSION 3.22.1)
project(UsbAudioReceiver LANGUAGES CXX)

include(CheckIPOSupported)
check_ipo_supported(RESULT UsbAudioIPOSupported LANGUAGES CXX)

add_library(usb_audio_native SHARED Receiver.cpp)
target_compile_features(usb_audio_native PRIVATE cxx_std_20)
target_include_directories(usb_audio_native PRIVATE ../../../../../shared)
target_link_libraries(usb_audio_native PRIVATE aaudio log android)
target_compile_options(usb_audio_native PRIVATE
    -Wall -Wextra -Werror=return-type
    -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections
    $<$<CONFIG:Release>:-O3>
)
target_link_options(usb_audio_native PRIVATE
    $<$<CONFIG:Release>:-Wl,--gc-sections>
    $<$<CONFIG:Release>:-Wl,--icf=safe>
    $<$<CONFIG:Release>:-Wl,--lto-O3>
)
set_target_properties(usb_audio_native PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(UsbAudioIPOSupported)
    set_property(TARGET usb_audio_native PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
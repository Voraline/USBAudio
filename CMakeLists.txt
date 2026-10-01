cmake_minimum_required(VERSION 3.24)
project(UsbAudioSender LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    string(REPLACE "/Ob2" "/Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT UsbAudioIPOSupported LANGUAGES CXX)

add_executable(UsbAudioSender
    windows/src/Main.cpp
    windows/src/AudioCapture.cpp
    windows/src/LowLatencyRender.cpp
    windows/src/UsbAccessory.cpp
)

target_compile_features(UsbAudioSender PRIVATE cxx_std_20)
target_include_directories(UsbAudioSender PRIVATE shared windows/src)
target_link_libraries(UsbAudioSender PRIVATE ole32 uuid avrt setupapi winusb advapi32)
target_compile_definitions(UsbAudioSender PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(UsbAudioSender PRIVATE
        /W4 /EHsc /arch:AVX2 /MP /fp:precise /Gy
        $<$<CONFIG:Release,RelWithDebInfo>:/Gw>
        $<$<CONFIG:Release,RelWithDebInfo>:/Zc:inline>
    )
    target_link_options(UsbAudioSender PRIVATE
        $<$<CONFIG:Release,RelWithDebInfo>:/OPT:REF>
        $<$<CONFIG:Release,RelWithDebInfo>:/OPT:ICF>
        $<$<CONFIG:Release,RelWithDebInfo>:/INCREMENTAL:NO>
    )
    if(UsbAudioIPOSupported)
        set_property(TARGET UsbAudioSender PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET UsbAudioSender PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
endif()

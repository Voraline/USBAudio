#pragma once

#include <cstddef>
#include <cstdint>

namespace UsbAudio
{
    inline constexpr std::uint8_t ReceiverReady = 1;
    inline constexpr std::size_t MaxPacketSamples = 480;
    inline constexpr std::uint32_t SampleRate = 48000;
    inline constexpr std::uint16_t Channels = 1;
}

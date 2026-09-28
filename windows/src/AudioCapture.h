#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

#include "AudioPacket.h"

class AudioCapture
{
public:
    bool Run(std::atomic<bool>& Running, SpscQueue<AudioPacket, 17>& Queue, HANDLE QueueEvent);

private:
    void ConvertFrame(const float* Samples, std::uint32_t ChannelCount, std::uint32_t SourceRate, std::atomic<bool>& Running, SpscQueue<AudioPacket, 17>& Queue);
    void EmitFrame(std::atomic<bool>& Running, SpscQueue<AudioPacket, 17>& Queue);

    void* Encoder = nullptr;
    AudioPacket CurrentPacket{};
    std::array<std::int16_t, UsbAudio::AudioFrameSamples * UsbAudio::Channels> PcmFrame{};
    std::size_t PcmFramePosition = 0;
    std::uint32_t Sequence = 0;
    double ResamplePhase = 0.0;
    float PreviousLeft = 0.0f;
    float PreviousRight = 0.0f;
    bool HasPrevious = false;
    HANDLE QueueEvent = nullptr;
};

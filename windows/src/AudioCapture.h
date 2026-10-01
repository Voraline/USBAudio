#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <windows.h>

#include "AudioPacket.h"

class AudioCapture
{
public:
    bool Run(std::atomic<bool>& Running, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue, HANDLE QueueEvent);

private:
    void ConvertFrame(float Mono, std::uint32_t SourceRate, std::atomic<bool>& Running, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue);
    void EmitFrame(SpscQueue<AudioPacket, AudioQueueCapacity>& Queue);

    static constexpr std::size_t MonoScratchCapacity = 4096;

    AudioPacket PcmFrame{};
    std::size_t PcmFramePosition = 0;
    double ResamplePhase = 0.0;
    float PreviousSample = 0.0f;
    bool HasPrevious = false;
    HANDLE QueueEvent = nullptr;
    std::array<float, MonoScratchCapacity> MonoScratch{};
};

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <windows.h>
#include <audioclient.h>

#include "AudioPacket.h"
#include "TpdfDither.h"

enum class CaptureResult
{
    Stopped,
    DeviceChanged,
    Failed
};

class AudioCapture
{
public:
    CaptureResult Run(std::atomic<bool>& Running, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue, HANDLE QueueEventHandle);

private:
    struct SourceFormat
    {
        std::uint32_t SampleRate = 0;
        std::uint32_t BlockAlign = 0;
        std::uint16_t Bits = 0;
        std::uint16_t Channels = 0;
        bool IsFloat = false;
        bool UseAvx2 = false;
    };

    HRESULT DrainPackets(IAudioCaptureClient* Capture, std::atomic<bool>& Running, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue);
    void ConvertBuffer(const std::uint8_t* Data, UINT32 Frames, DWORD Flags, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue);
    void ConvertFrame(float Mono, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue);
    void AppendSample(std::int16_t Sample, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue);
    void FlushPacket(SpscQueue<AudioPacket, AudioQueueCapacity>& Queue);
    std::int16_t ToPcm16(float Value);

    static constexpr std::size_t MonoScratchCapacity = 4096;
    static constexpr DWORD WaitTimeoutMs = 100;
    static constexpr std::size_t WaitHandleCapacity = 3;
    static constexpr DWORD NoWaitIndex = 0xFFFFFFFFu;

    SourceFormat Format;
    AudioPacket Packet{};
    TpdfDither Dither;
    double ResamplePhase = 0.0;
    float PreviousSample = 0.0f;
    bool HasPrevious = false;
    HANDLE QueueEvent = nullptr;
    std::array<float, MonoScratchCapacity> MonoScratch{};
};

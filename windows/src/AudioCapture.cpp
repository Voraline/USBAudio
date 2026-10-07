#include "AudioCapture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>

#include <immintrin.h>

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>

namespace
{
    const GUID& GetIeeeFloatSubtype()
    {
        static const GUID IeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        return IeeeFloat;
    }
}

#include "DefaultDeviceWatcher.h"
#include "LowLatencyRender.h"

namespace
{
    float ReadSample(const std::uint8_t* Data, std::uint16_t Bits, bool IsFloat)
    {
        if (IsFloat && Bits == 32)
        {
            float Value = 0.0f;
            std::memcpy(&Value, Data, sizeof(Value));
            return std::isfinite(Value) ? Value : 0.0f;
        }
        if (Bits == 16)
        {
            std::int16_t Value = 0;
            std::memcpy(&Value, Data, sizeof(Value));
            return static_cast<float>(Value) / 32768.0f;
        }
        if (Bits == 24)
        {
            std::int32_t Value = static_cast<std::int32_t>(Data[0]) |
                (static_cast<std::int32_t>(Data[1]) << 8) |
                (static_cast<std::int32_t>(Data[2]) << 16);
            if ((Value & 0x800000) != 0)
            {
                Value |= static_cast<std::int32_t>(0xFF000000);
            }
            return static_cast<float>(Value) / 8388608.0f;
        }
        if (Bits == 32)
        {
            std::int32_t Value = 0;
            std::memcpy(&Value, Data, sizeof(Value));
            return static_cast<float>(Value) / 2147483648.0f;
        }
        return 0.0f;
    }

    bool IsFloatFormat(const WAVEFORMATEX* Format)
    {
        if (Format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        {
            return true;
        }
        if (Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && Format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            const auto* Extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(Format);
            return IsEqualGUID(Extended->SubFormat, GetIeeeFloatSubtype()) != 0;
        }
        return false;
    }

    void DownmixMonoFloat32Stereo(const std::uint8_t* Bytes, UINT32 Frames, float* Mono)
    {
        const __m256 Half = _mm256_set1_ps(0.5f);
        const __m256 AbsMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
        const __m256 InfinityMagnitude = _mm256_set1_ps(std::numeric_limits<float>::infinity());
        UINT32 Frame = 0;
        for (; Frame + 8 <= Frames; Frame += 8)
        {
            const float* Source = reinterpret_cast<const float*>(Bytes + static_cast<std::size_t>(Frame) * 8);
            __m256 First = _mm256_loadu_ps(Source);
            __m256 Second = _mm256_loadu_ps(Source + 8);
            First = _mm256_and_ps(First, _mm256_cmp_ps(_mm256_and_ps(First, AbsMask), InfinityMagnitude, _CMP_LT_OQ));
            Second = _mm256_and_ps(Second, _mm256_cmp_ps(_mm256_and_ps(Second, AbsMask), InfinityMagnitude, _CMP_LT_OQ));
            const __m256 Sums = _mm256_hadd_ps(First, Second);
            const __m256 Ordered = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(Sums), 0xD8));
            _mm256_storeu_ps(Mono + Frame, _mm256_mul_ps(Ordered, Half));
        }
        for (; Frame < Frames; ++Frame)
        {
            const std::uint8_t* FrameData = Bytes + static_cast<std::size_t>(Frame) * 8;
            float Left = 0.0f;
            float Right = 0.0f;
            std::memcpy(&Left, FrameData, sizeof(Left));
            std::memcpy(&Right, FrameData + sizeof(float), sizeof(Right));
            Left = std::isfinite(Left) ? Left : 0.0f;
            Right = std::isfinite(Right) ? Right : 0.0f;
            Mono[Frame] = (Left + Right) * 0.5f;
        }
    }

    void DownmixMonoScalar(const std::uint8_t* Bytes, UINT32 Frames, std::uint32_t BlockAlign, std::uint16_t Bits, bool IsFloat, std::uint16_t Channels, float* Mono)
    {
        const std::size_t BytesPerSample = Bits / 8;
        for (UINT32 Frame = 0; Frame < Frames; ++Frame)
        {
            const std::uint8_t* FrameData = Bytes + static_cast<std::size_t>(Frame) * BlockAlign;
            const float Left = ReadSample(FrameData, Bits, IsFloat);
            const float Right = Channels > 1 ? ReadSample(FrameData + BytesPerSample, Bits, IsFloat) : Left;
            Mono[Frame] = (Left + Right) * 0.5f;
        }
    }
}

CaptureResult AudioCapture::Run(std::atomic<bool>& Running, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue, HANDLE QueueEventHandle)
{
    QueueEvent = QueueEventHandle;
    HRESULT Result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(Result) && Result != RPC_E_CHANGED_MODE)
    {
        return CaptureResult::Failed;
    }
    const bool ShouldUninitialize = SUCCEEDED(Result);

    IMMDeviceEnumerator* Enumerator = nullptr;
    IMMDevice* Device = nullptr;
    IAudioClient* Client = nullptr;
    IAudioCaptureClient* Capture = nullptr;
    WAVEFORMATEX* MixFormat = nullptr;
    HANDLE AudioEvent = nullptr;
    DefaultDeviceWatcher* Watcher = nullptr;
    LowLatencyRender LowLatency;
    DWORD TaskIndex = 0;
    HANDLE MmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &TaskIndex);
    if (MmcssHandle != nullptr)
    {
        AvSetMmThreadPriority(MmcssHandle, AVRT_PRIORITY_HIGH);
    }

    auto Release = [&]()
    {
        if (Client != nullptr)
        {
            Client->Stop();
        }
        if (Capture != nullptr)
        {
            Capture->Release();
        }
        if (Client != nullptr)
        {
            Client->Release();
        }
        if (Device != nullptr)
        {
            Device->Release();
        }
        LowLatency.Close();
        if (Watcher != nullptr)
        {
            Watcher->Stop();
            Watcher->Release();
            Watcher = nullptr;
        }
        if (Enumerator != nullptr)
        {
            Enumerator->Release();
        }
        if (MixFormat != nullptr)
        {
            CoTaskMemFree(MixFormat);
        }
        if (AudioEvent != nullptr)
        {
            CloseHandle(AudioEvent);
        }
        if (MmcssHandle != nullptr)
        {
            AvRevertMmThreadCharacteristics(MmcssHandle);
            MmcssHandle = nullptr;
        }
        if (ShouldUninitialize)
        {
            CoUninitialize();
        }
    };

    Result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&Enumerator));
    if (FAILED(Result))
    {
        Release();
        return CaptureResult::Failed;
    }
    Watcher = new (std::nothrow) DefaultDeviceWatcher;
    if (Watcher != nullptr && !Watcher->Start(Enumerator))
    {
        Watcher->Release();
        Watcher = nullptr;
    }
    if (FAILED(Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Device)) ||
        FAILED(Device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&Client))) ||
        FAILED(Client->GetMixFormat(&MixFormat)))
    {
        Release();
        return CaptureResult::Failed;
    }
    if (MixFormat->nChannels == 0 || MixFormat->nSamplesPerSec == 0 || MixFormat->nBlockAlign == 0)
    {
        Release();
        return CaptureResult::Failed;
    }
    Format.SampleRate = MixFormat->nSamplesPerSec;
    Format.BlockAlign = MixFormat->nBlockAlign;
    Format.Bits = MixFormat->wBitsPerSample;
    Format.Channels = MixFormat->nChannels;
    Format.IsFloat = IsFloatFormat(MixFormat);
    if ((!Format.IsFloat && Format.Bits != 16 && Format.Bits != 24 && Format.Bits != 32) || (Format.IsFloat && Format.Bits != 32))
    {
        Release();
        return CaptureResult::Failed;
    }
    Format.UseAvx2 = Format.IsFloat && Format.Bits == 32 && Format.Channels == 2 && Format.BlockAlign == 8;
    ResampleStep = static_cast<double>(Format.SampleRate) / static_cast<double>(UsbAudio::SampleRate);
    Resampler.Reset();

    std::wcout << (LowLatency.Open(Device, MixFormat) ? L"Capture mode: low latency\n" : L"Capture mode: default\n");
    AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (AudioEvent == nullptr || FAILED(Client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, MixFormat, nullptr)) ||
        FAILED(Client->SetEventHandle(AudioEvent)) || FAILED(Client->GetService(IID_PPV_ARGS(&Capture))) || FAILED(Client->Start()))
    {
        Release();
        return CaptureResult::Failed;
    }

    HANDLE WaitHandles[WaitHandleCapacity]{};
    DWORD WaitCount = 0;
    DWORD DeviceIndex = NoWaitIndex;
    DWORD RenderIndex = NoWaitIndex;
    const DWORD AudioIndex = WaitCount;
    WaitHandles[WaitCount++] = AudioEvent;
    if (Watcher != nullptr)
    {
        DeviceIndex = WaitCount;
        WaitHandles[WaitCount++] = Watcher->GetEvent();
    }
    if (LowLatency.GetEvent() != nullptr)
    {
        RenderIndex = WaitCount;
        WaitHandles[WaitCount++] = LowLatency.GetEvent();
    }

    CaptureResult Outcome = CaptureResult::Stopped;
    while (Running.load(std::memory_order_acquire))
    {
        const DWORD WaitResult = WaitForMultipleObjects(WaitCount, WaitHandles, FALSE, WaitTimeoutMs);
        if (WaitResult == WAIT_TIMEOUT)
        {
            UINT32 ProbeFrames = 0;
            if (FAILED(Capture->GetNextPacketSize(&ProbeFrames)))
            {
                Outcome = CaptureResult::DeviceChanged;
                break;
            }
            continue;
        }
        if (WaitResult >= WAIT_OBJECT_0 + WaitCount)
        {
            Running.store(false, std::memory_order_release);
            break;
        }
        const DWORD Signaled = WaitResult - WAIT_OBJECT_0;
        if (Signaled == DeviceIndex)
        {
            Outcome = CaptureResult::DeviceChanged;
            break;
        }
        if (Signaled == RenderIndex)
        {
            LowLatency.FillSilence();
            continue;
        }
        if (Signaled != AudioIndex)
        {
            continue;
        }
        const HRESULT DrainResult = DrainPackets(Capture, Running, Queue);
        FlushPacket(Queue);
        if (FAILED(DrainResult))
        {
            if (DrainResult == AUDCLNT_E_DEVICE_INVALIDATED)
            {
                Outcome = CaptureResult::DeviceChanged;
            }
            else
            {
                Running.store(false, std::memory_order_release);
            }
            break;
        }
    }
    Release();
    return Outcome;
}

HRESULT AudioCapture::DrainPackets(IAudioCaptureClient* Capture, std::atomic<bool>& Running, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue)
{
    UINT32 PacketFrames = 0;
    HRESULT Result = Capture->GetNextPacketSize(&PacketFrames);
    while (SUCCEEDED(Result) && PacketFrames > 0 && Running.load(std::memory_order_acquire))
    {
        BYTE* Data = nullptr;
        UINT32 Frames = 0;
        DWORD Flags = 0;
        Result = Capture->GetBuffer(&Data, &Frames, &Flags, nullptr, nullptr);
        if (FAILED(Result))
        {
            return Result;
        }
        ConvertBuffer(reinterpret_cast<const std::uint8_t*>(Data), Frames, Flags, Queue);
        Capture->ReleaseBuffer(Frames);
        Result = Capture->GetNextPacketSize(&PacketFrames);
    }
    return Result;
}

void AudioCapture::PushSilentPacket(SpscQueue<AudioPacket, AudioQueueCapacity>& Queue)
{
    AudioPacket Silent{};
    Silent.Count = static_cast<std::uint16_t>(Silent.Samples.size());
    if (Queue.Push(Silent) && QueueEvent != nullptr)
    {
        SetEvent(QueueEvent);
    }
}

void AudioCapture::ConvertBuffer(const std::uint8_t* Data, UINT32 Frames, DWORD Flags, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue)
{
    const bool Silent = (Flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    if (Silent)
    {
        UINT32 Remaining = Frames;
        while (Remaining > 0)
        {
            const UINT32 Space = static_cast<UINT32>(Packet.Samples.size()) - Packet.Count;
            if (Remaining >= Space)
            {
                std::memset(Packet.Samples.data() + Packet.Count, 0, Space * sizeof(std::int16_t));
                Packet.Count = static_cast<std::uint16_t>(Packet.Samples.size());
                FlushPacket(Queue);
                Remaining -= Space;
            }
            else
            {
                std::memset(Packet.Samples.data() + Packet.Count, 0, Remaining * sizeof(std::int16_t));
                Packet.Count += static_cast<std::uint16_t>(Remaining);
                Remaining = 0;
            }
        }
        return;
    }

    UINT32 FramesDone = 0;
    while (FramesDone < Frames)
    {
        const UINT32 ChunkFrames = std::min<UINT32>(Frames - FramesDone, static_cast<UINT32>(MonoScratch.size()));
        const std::uint8_t* ChunkBytes = Data + static_cast<std::size_t>(FramesDone) * Format.BlockAlign;
        if (Format.UseAvx2)
        {
            DownmixMonoFloat32Stereo(ChunkBytes, ChunkFrames, MonoScratch.data());
        }
        else
        {
            DownmixMonoScalar(ChunkBytes, ChunkFrames, Format.BlockAlign, Format.Bits, Format.IsFloat, Format.Channels, MonoScratch.data());
        }
        for (UINT32 Frame = 0; Frame < ChunkFrames; ++Frame)
        {
            ConvertFrame(MonoScratch[Frame], Queue);
        }
        FramesDone += ChunkFrames;
    }
}

void AudioCapture::ConvertFrame(float Mono, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue)
{
    if (Format.SampleRate == UsbAudio::SampleRate)
    {
        AppendSample(ToPcm16(Mono), Queue);
        return;
    }
    Resampler.Push(Mono, ResampleStep, [&](float Sample)
    {
        AppendSample(ToPcm16(Sample), Queue);
    });
}

void AudioCapture::AppendSample(std::int16_t Sample, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue)
{
    Packet.Samples[Packet.Count++] = Sample;
    if (Packet.Count == Packet.Samples.size())
    {
        FlushPacket(Queue);
    }
}

void AudioCapture::FlushPacket(SpscQueue<AudioPacket, AudioQueueCapacity>& Queue)
{
    if (Packet.Count == 0)
    {
        return;
    }
    if (Queue.Push(Packet) && QueueEvent != nullptr)
    {
        SetEvent(QueueEvent);
    }
    Packet.Count = 0;
}

std::int16_t AudioCapture::ToPcm16(float Value)
{
    if (Value == 0.0f)
    {
        return 0;
    }
    const float Scaled = std::clamp(Value, -1.0f, 1.0f) * 32767.0f + Dither.Next();
    return static_cast<std::int16_t>(std::clamp<long>(std::lrintf(Scaled), -32768L, 32767L));
}

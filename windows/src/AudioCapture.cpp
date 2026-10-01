#include "AudioCapture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

#include <immintrin.h>

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include "LowLatencyRender.h"

namespace
{
    float ClampSample(float Value)
    {
        return std::clamp(Value, -1.0f, 1.0f);
    }

    std::int16_t ToPcm16(float Value)
    {
        const float Clamped = ClampSample(Value);
        return static_cast<std::int16_t>(std::lrint(Clamped * 32767.0f));
    }

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
            return IsEqualGUID(Extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
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

bool AudioCapture::Run(std::atomic<bool>& Running, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue, HANDLE QueueEventHandle)
{
    QueueEvent = QueueEventHandle;
    HRESULT Result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(Result) && Result != RPC_E_CHANGED_MODE)
    {
        return false;
    }
    const bool ShouldUninitialize = SUCCEEDED(Result);

    IMMDeviceEnumerator* Enumerator = nullptr;
    IMMDevice* Device = nullptr;
    IAudioClient* Client = nullptr;
    IAudioCaptureClient* Capture = nullptr;
    WAVEFORMATEX* MixFormat = nullptr;
    HANDLE AudioEvent = nullptr;
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
    if (FAILED(Result) || FAILED(Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Device)) ||
        FAILED(Device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&Client))) ||
        FAILED(Client->GetMixFormat(&MixFormat)))
    {
        Release();
        return false;
    }
    if (MixFormat->nChannels == 0 || MixFormat->nSamplesPerSec == 0 || MixFormat->nBlockAlign == 0)
    {
        Release();
        return false;
    }
    const bool IsFloat = IsFloatFormat(MixFormat);
    const std::uint16_t Bits = MixFormat->wBitsPerSample;
    if ((!IsFloat && Bits != 16 && Bits != 24 && Bits != 32) || (IsFloat && Bits != 32))
    {
        Release();
        return false;
    }
    const bool UseAvx2Downmix = IsFloat && Bits == 32 && MixFormat->nChannels == 2 && MixFormat->nBlockAlign == 8;

    std::wcout << (LowLatency.Open(Device, MixFormat) ? L"Capture mode: low latency\n" : L"Capture mode: default\n");
    AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (AudioEvent == nullptr || FAILED(Client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, MixFormat, nullptr)) ||
        FAILED(Client->SetEventHandle(AudioEvent)) || FAILED(Client->GetService(IID_PPV_ARGS(&Capture))) || FAILED(Client->Start()))
    {
        Release();
        return false;
    }

    HANDLE WaitHandles[] = { AudioEvent, LowLatency.GetEvent() };
    const DWORD WaitCount = WaitHandles[1] != nullptr ? 2 : 1;
    while (Running.load(std::memory_order_acquire))
    {
        const DWORD WaitResult = WaitForMultipleObjects(WaitCount, WaitHandles, FALSE, 100);
        if (WaitResult == WAIT_TIMEOUT)
        {
            continue;
        }
        if (WaitResult == WAIT_OBJECT_0 + 1)
        {
            LowLatency.FillSilence();
            continue;
        }
        if (WaitResult != WAIT_OBJECT_0)
        {
            Running.store(false, std::memory_order_release);
            break;
        }
        UINT32 PacketFrames = 0;
        while (SUCCEEDED(Capture->GetNextPacketSize(&PacketFrames)) && PacketFrames > 0)
        {
            BYTE* Data = nullptr;
            UINT32 Frames = 0;
            DWORD Flags = 0;
            Result = Capture->GetBuffer(&Data, &Frames, &Flags, nullptr, nullptr);
            if (FAILED(Result))
            {
                Running.store(false, std::memory_order_release);
                break;
            }
            const auto* Bytes = reinterpret_cast<const std::uint8_t*>(Data);
            UINT32 FramesDone = 0;
            while (FramesDone < Frames && Running.load(std::memory_order_relaxed))
            {
                const UINT32 ChunkFrames = std::min<UINT32>(Frames - FramesDone, static_cast<UINT32>(MonoScratch.size()));
                if ((Flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
                {
                    std::fill_n(MonoScratch.begin(), ChunkFrames, 0.0f);
                }
                else
                {
                    const std::uint8_t* ChunkBytes = Bytes + static_cast<std::size_t>(FramesDone) * MixFormat->nBlockAlign;
                    if (UseAvx2Downmix)
                    {
                        DownmixMonoFloat32Stereo(ChunkBytes, ChunkFrames, MonoScratch.data());
                    }
                    else
                    {
                        DownmixMonoScalar(ChunkBytes, ChunkFrames, MixFormat->nBlockAlign, Bits, IsFloat, MixFormat->nChannels, MonoScratch.data());
                    }
                }
                for (UINT32 Frame = 0; Frame < ChunkFrames && Running.load(std::memory_order_relaxed); ++Frame)
                {
                    ConvertFrame(MonoScratch[Frame], MixFormat->nSamplesPerSec, Running, Queue);
                }
                FramesDone += ChunkFrames;
            }
            Capture->ReleaseBuffer(Frames);
            if (!Running.load(std::memory_order_acquire))
            {
                break;
            }
        }
    }
    Release();
    return true;
}

void AudioCapture::ConvertFrame(float Mono, std::uint32_t SourceRate, std::atomic<bool>& Running, SpscQueue<AudioPacket, AudioQueueCapacity>& Queue)
{
    if (SourceRate == UsbAudio::SampleRate)
    {
        PcmFrame[PcmFramePosition] = ToPcm16(Mono);
        if (++PcmFramePosition == UsbAudio::AudioFrameSamples)
        {
            EmitFrame(Queue);
        }
        return;
    }
    const double Step = static_cast<double>(SourceRate) / UsbAudio::SampleRate;
    if (!HasPrevious)
    {
        PreviousSample = Mono;
        HasPrevious = true;
        PcmFrame[PcmFramePosition] = ToPcm16(Mono);
        ++PcmFramePosition;
        ResamplePhase = Step;
    }
    else
    {
        while (ResamplePhase <= 1.0 && Running.load(std::memory_order_relaxed))
        {
            const float Ratio = static_cast<float>(std::clamp(ResamplePhase, 0.0, 1.0));
            PcmFrame[PcmFramePosition] = ToPcm16(PreviousSample + (Mono - PreviousSample) * Ratio);
            ++PcmFramePosition;
            if (PcmFramePosition == UsbAudio::AudioFrameSamples)
            {
                EmitFrame(Queue);
            }
            ResamplePhase += Step;
        }
        ResamplePhase -= 1.0;
        PreviousSample = Mono;
    }
}

void AudioCapture::EmitFrame(SpscQueue<AudioPacket, AudioQueueCapacity>& Queue)
{
    PcmFramePosition = 0;
    if (Queue.Push(PcmFrame) && QueueEvent != nullptr)
    {
        SetEvent(QueueEvent);
    }
}

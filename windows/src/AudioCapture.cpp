#include "AudioCapture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <opus/opus.h>
#include <propidl.h>
#include <windows.h>

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

    float ReadSample(const std::uint8_t* Data, const WAVEFORMATEX* Format, std::uint16_t Bits, bool IsFloat)
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
        static_cast<void>(Format);
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

    std::uint16_t GetBitsPerSample(const WAVEFORMATEX* Format)
    {
        return Format->wBitsPerSample;
    }

    std::uint64_t GetTimestampNs()
    {
        LARGE_INTEGER Counter{};
        LARGE_INTEGER Frequency{};
        QueryPerformanceCounter(&Counter);
        QueryPerformanceFrequency(&Frequency);
        return static_cast<std::uint64_t>((static_cast<long double>(Counter.QuadPart) * 1000000000.0L) / Frequency.QuadPart);
    }
}

bool AudioCapture::Run(std::atomic<bool>& Running, SpscQueue<AudioPacket, 17>& Queue, HANDLE QueueEventHandle)
{
    QueueEvent = QueueEventHandle;
    HRESULT Result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(Result) && Result != RPC_E_CHANGED_MODE)
    {
        std::cerr << "Could not initialize the Windows audio capture thread.\n";
        return false;
    }
    const bool ShouldUninitialize = SUCCEEDED(Result);

    IMMDeviceEnumerator* Enumerator = nullptr;
    IMMDevice* Device = nullptr;
    IAudioClient* Client = nullptr;
    IAudioCaptureClient* Capture = nullptr;
    WAVEFORMATEX* MixFormat = nullptr;
    HANDLE AudioEvent = nullptr;
    int OpusError = OPUS_OK;
    Encoder = opus_encoder_create(UsbAudio::SampleRate, UsbAudio::Channels, OPUS_APPLICATION_AUDIO, &OpusError);
    if (OpusError != OPUS_OK || Encoder == nullptr)
    {
        std::cerr << "Could not create the Opus encoder.\n";
        if (ShouldUninitialize)
        {
            CoUninitialize();
        }
        return false;
    }
    opus_encoder_ctl(static_cast<OpusEncoder*>(Encoder), OPUS_SET_BITRATE(128000));
    opus_encoder_ctl(static_cast<OpusEncoder*>(Encoder), OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(static_cast<OpusEncoder*>(Encoder), OPUS_SET_VBR(1));
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
        if (Encoder != nullptr)
        {
            opus_encoder_destroy(static_cast<OpusEncoder*>(Encoder));
            Encoder = nullptr;
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
        std::cerr << "Could not open the default Windows playback device.\n";
        Release();
        return false;
    }
    if (MixFormat->nChannels == 0 || MixFormat->nSamplesPerSec == 0 || MixFormat->nBlockAlign == 0)
    {
        std::cerr << "The default Windows playback format is not supported.\n";
        Release();
        return false;
    }
    const bool IsFloat = IsFloatFormat(MixFormat);
    const std::uint16_t Bits = GetBitsPerSample(MixFormat);
    if ((!IsFloat && Bits != 16 && Bits != 24 && Bits != 32) || (IsFloat && Bits != 32))
    {
        std::cerr << "The default playback format must use 16, 24, or 32-bit PCM or 32-bit float.\n";
        Release();
        return false;
    }

    AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (AudioEvent == nullptr || FAILED(Client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, MixFormat, nullptr)) ||
        FAILED(Client->SetEventHandle(AudioEvent)) || FAILED(Client->GetService(IID_PPV_ARGS(&Capture))) || FAILED(Client->Start()))
    {
        std::cerr << "Could not start Windows loopback capture.\n";
        Release();
        return false;
    }

    std::cout << "Capturing system audio at " << MixFormat->nSamplesPerSec << " Hz from " << MixFormat->nChannels << " channels.\n";
    HANDLE WaitHandles[] = { AudioEvent };
    while (Running.load(std::memory_order_acquire))
    {
        const DWORD WaitResult = WaitForMultipleObjects(1, WaitHandles, FALSE, 100);
        if (WaitResult == WAIT_TIMEOUT)
        {
            continue;
        }
        if (WaitResult != WAIT_OBJECT_0)
        {
            std::cerr << "Windows audio capture wait failed.\n";
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
                std::cerr << "Windows loopback capture stopped.\n";
                Running.store(false, std::memory_order_release);
                break;
            }
            if ((Flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
            {
                for (UINT32 Frame = 0; Frame < Frames; ++Frame)
                {
                    const float Silent[2] = { 0.0f, 0.0f };
                    ConvertFrame(Silent, MixFormat->nChannels, MixFormat->nSamplesPerSec, Running, Queue);
                }
            }
            else
            {
                const auto* Bytes = reinterpret_cast<const std::uint8_t*>(Data);
                const std::size_t BytesPerSample = Bits / 8;
                for (UINT32 Frame = 0; Frame < Frames && Running.load(std::memory_order_relaxed); ++Frame)
                {
                    const std::uint8_t* FrameData = Bytes + static_cast<std::size_t>(Frame) * MixFormat->nBlockAlign;
                    const float Left = ReadSample(FrameData, MixFormat, Bits, IsFloat);
                    const float Right = MixFormat->nChannels > 1
                        ? ReadSample(FrameData + BytesPerSample, MixFormat, Bits, IsFloat)
                        : Left;
                    const float Stereo[2] = { Left, Right };
                    ConvertFrame(Stereo, 2, MixFormat->nSamplesPerSec, Running, Queue);
                }
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

void AudioCapture::ConvertFrame(const float* Samples, std::uint32_t ChannelCount, std::uint32_t SourceRate, std::atomic<bool>& Running, SpscQueue<AudioPacket, 17>& Queue)
{
    const float Left = Samples[0];
    const float Right = ChannelCount > 1 ? Samples[1] : Left;
    const double Step = static_cast<double>(SourceRate) / UsbAudio::SampleRate;
    if (!HasPrevious)
    {
        PreviousLeft = Left;
        PreviousRight = Right;
        HasPrevious = true;
        PcmFrame[PcmFramePosition * 2] = ToPcm16(Left);
        PcmFrame[PcmFramePosition * 2 + 1] = ToPcm16(Right);
        ++PcmFramePosition;
        ResamplePhase = Step;
    }
    else
    {
        while (ResamplePhase <= 1.0 && Running.load(std::memory_order_relaxed))
        {
            const float Ratio = static_cast<float>(std::clamp(ResamplePhase, 0.0, 1.0));
            PcmFrame[PcmFramePosition * 2] = ToPcm16(PreviousLeft + (Left - PreviousLeft) * Ratio);
            PcmFrame[PcmFramePosition * 2 + 1] = ToPcm16(PreviousRight + (Right - PreviousRight) * Ratio);
            ++PcmFramePosition;
            if (PcmFramePosition == UsbAudio::AudioFrameSamples)
            {
                EmitFrame(Running, Queue);
            }
            ResamplePhase += Step;
        }
        ResamplePhase -= 1.0;
        PreviousLeft = Left;
        PreviousRight = Right;
    }
}

void AudioCapture::EmitFrame(std::atomic<bool>& Running, SpscQueue<AudioPacket, 17>& Queue)
{
    CurrentPacket.Sequence = Sequence++;
    CurrentPacket.TimestampNs = GetTimestampNs();
    const int EncodedSize = opus_encode(static_cast<OpusEncoder*>(Encoder), PcmFrame.data(),
        static_cast<int>(UsbAudio::AudioFrameSamples), CurrentPacket.Payload.data(),
        static_cast<opus_int32>(CurrentPacket.Payload.size()));
    PcmFramePosition = 0;
    if (EncodedSize < 0)
    {
        std::cerr << "Opus encoding failed with error " << EncodedSize << ".\n";
        Running.store(false, std::memory_order_release);
        return;
    }
    CurrentPacket.PayloadSize = static_cast<std::uint16_t>(EncodedSize);
    if (!Queue.Push(CurrentPacket))
    {
        static std::atomic<std::uint32_t> DroppedPackets{0};
        if ((DroppedPackets.fetch_add(1, std::memory_order_relaxed) % 100) == 0)
        {
            std::cerr << "USB sender is behind; dropping captured audio packets.\n";
        }
    }
    else if (QueueEvent != nullptr)
    {
        SetEvent(QueueEvent);
    }
}

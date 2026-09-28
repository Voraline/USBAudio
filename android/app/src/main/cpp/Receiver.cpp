#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include <aaudio/AAudio.h>
#include <jni.h>
#include <opus.h>

#include "Protocol.h"

class PcmRing
{
public:
    static constexpr std::uint64_t CapacityFrames = 16384;

    bool Write(const std::int16_t* Samples, std::size_t Frames)
    {
        const std::uint64_t WriteFrame = WriteIndex.load(std::memory_order_relaxed);
        const std::uint64_t ReadFrame = ReadIndex.load(std::memory_order_acquire);
        if (Frames > CapacityFrames - std::min<std::uint64_t>(WriteFrame - ReadFrame, CapacityFrames))
        {
            return false;
        }
        for (std::size_t Frame = 0; Frame < Frames; ++Frame)
        {
            const std::size_t Slot = static_cast<std::size_t>((WriteFrame + Frame) & (CapacityFrames - 1));
            SamplesData[Slot * 2] = Samples[Frame * 2];
            SamplesData[Slot * 2 + 1] = Samples[Frame * 2 + 1];
        }
        WriteIndex.store(WriteFrame + Frames, std::memory_order_release);
        return true;
    }

    std::uint64_t GetWriteIndex() const
    {
        return WriteIndex.load(std::memory_order_acquire);
    }

    std::uint64_t GetReadIndex() const
    {
        return ReadIndex.load(std::memory_order_acquire);
    }

    void PublishReadIndex(std::uint64_t Value)
    {
        ReadIndex.store(Value, std::memory_order_release);
    }

    const std::int16_t* GetFrame(std::uint64_t Index) const
    {
        return SamplesData.data() + static_cast<std::size_t>(Index & (CapacityFrames - 1)) * 2;
    }

    std::uint32_t GetQueuedFrames() const
    {
        const std::uint64_t WriteFrame = WriteIndex.load(std::memory_order_acquire);
        const std::uint64_t ReadFrame = ReadIndex.load(std::memory_order_acquire);
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(WriteFrame - ReadFrame, CapacityFrames));
    }

private:
    std::array<std::int16_t, CapacityFrames * 2> SamplesData{};
    alignas(64) std::atomic<std::uint64_t> WriteIndex{0};
    alignas(64) std::atomic<std::uint64_t> ReadIndex{0};
};

class Receiver
{
public:
    explicit Receiver(int AccessoryDescriptor)
        : Descriptor(AccessoryDescriptor)
    {
    }

    ~Receiver()
    {
        Stop();
    }

    bool Start()
    {
        if (Descriptor < 0)
        {
            return false;
        }
        const int Flags = fcntl(Descriptor, F_GETFL, 0);
        if (Flags < 0 || fcntl(Descriptor, F_SETFL, Flags | O_NONBLOCK) < 0)
        {
            Stop();
            return false;
        }
        int Error = OPUS_OK;
        Decoder = opus_decoder_create(UsbAudio::SampleRate, UsbAudio::Channels, &Error);
        if (Decoder == nullptr || Error != OPUS_OK)
        {
            Stop();
            return false;
        }
        if (!OpenAudioStream())
        {
            Stop();
            return false;
        }
        Running.store(true, std::memory_order_release);
        TransportThread = std::thread(&Receiver::RunTransport, this);
        return true;
    }

    void Stop()
    {
        Running.store(false, std::memory_order_release);
        if (TransportThread.joinable())
        {
            TransportThread.join();
        }
        if (Stream != nullptr)
        {
            AAudioStream_requestStop(Stream);
            AAudioStream_close(Stream);
            Stream = nullptr;
        }
        if (Decoder != nullptr)
        {
            opus_decoder_destroy(Decoder);
            Decoder = nullptr;
        }
        if (Descriptor >= 0)
        {
            close(Descriptor);
            Descriptor = -1;
        }
    }

    bool IsRunning() const
    {
        return Running.load(std::memory_order_acquire);
    }

private:
    static aaudio_data_callback_result_t AudioCallback(AAudioStream*, void* UserData, void* AudioData, std::int32_t NumFrames)
    {
        auto* Self = static_cast<Receiver*>(UserData);
        Self->Render(static_cast<std::int16_t*>(AudioData), NumFrames);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    bool OpenAudioStream()
    {
        AAudioStreamBuilder* Builder = nullptr;
        if (AAudio_createStreamBuilder(&Builder) != AAUDIO_OK || Builder == nullptr)
        {
            return false;
        }
        AAudioStreamBuilder_setDirection(Builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(Builder, UsbAudio::SampleRate);
        AAudioStreamBuilder_setChannelCount(Builder, UsbAudio::Channels);
        AAudioStreamBuilder_setFormat(Builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(Builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(Builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setUsage(Builder, AAUDIO_USAGE_MEDIA);
        AAudioStreamBuilder_setContentType(Builder, AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setDataCallback(Builder, &Receiver::AudioCallback, this);
        const aaudio_result_t OpenResult = AAudioStreamBuilder_openStream(Builder, &Stream);
        AAudioStreamBuilder_delete(Builder);
        if (OpenResult != AAUDIO_OK || Stream == nullptr)
        {
            Stream = nullptr;
            return false;
        }
        const std::int32_t BurstFrames = AAudioStream_getFramesPerBurst(Stream);
        if (BurstFrames > 0)
        {
            AAudioStream_setBufferSizeInFrames(Stream, std::max<std::int32_t>(BurstFrames * 2, 192));
        }
        return AAudioStream_requestStart(Stream) == AAUDIO_OK;
    }

    void Render(std::int16_t* Output, std::int32_t NumFrames)
    {
        const std::uint64_t WriteFrame = Ring.GetWriteIndex();
        const std::uint64_t PublishedRead = Ring.GetReadIndex();
        if (!PlaybackStarted)
        {
            ReadPosition = static_cast<double>(PublishedRead);
            if (WriteFrame - PublishedRead < StartupFrames)
            {
                std::memset(Output, 0, static_cast<std::size_t>(NumFrames) * UsbAudio::Channels * sizeof(std::int16_t));
                return;
            }
            PlaybackStarted = true;
        }
        const std::uint64_t CurrentFrame = static_cast<std::uint64_t>(ReadPosition);
        const std::uint64_t FillFrames = WriteFrame > CurrentFrame ? WriteFrame - CurrentFrame : 0;
        const double Ratio = std::clamp(1.0 + (static_cast<double>(FillFrames) - TargetFrames) * 0.000001, 0.995, 1.005);
        std::int32_t OutputFrame = 0;
        std::uint32_t MissingFrames = 0;
        for (; OutputFrame < NumFrames; ++OutputFrame)
        {
            const std::uint64_t FrameIndex = static_cast<std::uint64_t>(ReadPosition);
            if (FrameIndex >= WriteFrame)
            {
                Output[OutputFrame * 2] = 0;
                Output[OutputFrame * 2 + 1] = 0;
                ++MissingFrames;
                continue;
            }
            const std::int16_t* First = Ring.GetFrame(FrameIndex);
            const std::int16_t* Second = FrameIndex + 1 < WriteFrame ? Ring.GetFrame(FrameIndex + 1) : First;
            const double Fraction = ReadPosition - static_cast<double>(FrameIndex);
            for (std::size_t Channel = 0; Channel < UsbAudio::Channels; ++Channel)
            {
                const double Sample = First[Channel] + (static_cast<double>(Second[Channel]) - First[Channel]) * Fraction;
                Output[OutputFrame * 2 + Channel] = static_cast<std::int16_t>(std::clamp(Sample, -32768.0, 32767.0));
            }
            ReadPosition += Ratio;
        }
        Ring.PublishReadIndex(static_cast<std::uint64_t>(ReadPosition));
        if (MissingFrames != 0)
        {
            Underruns.fetch_add(MissingFrames, std::memory_order_relaxed);
        }
    }

    bool WaitFor(short Events)
    {
        pollfd PollDescriptor{};
        PollDescriptor.fd = Descriptor;
        PollDescriptor.events = Events;
        while (Running.load(std::memory_order_acquire))
        {
            const int Result = poll(&PollDescriptor, 1, 100);
            if (Result > 0)
            {
                return (PollDescriptor.revents & Events) != 0 &&
                    (PollDescriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
            }
            if (Result < 0 && errno != EINTR)
            {
                return false;
            }
        }
        return false;
    }

    bool ReadExact(std::uint8_t* Data, std::size_t Size)
    {
        std::size_t Offset = 0;
        while (Offset < Size && Running.load(std::memory_order_acquire))
        {
            if (!WaitFor(POLLIN))
            {
                return false;
            }
            const ssize_t Count = read(Descriptor, Data + Offset, Size - Offset);
            if (Count > 0)
            {
                Offset += static_cast<std::size_t>(Count);
            }
            else if (Count == 0)
            {
                return false;
            }
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                return false;
            }
        }
        return Offset == Size;
    }

    bool ReadPacket(UsbAudio::PacketHeader& Header, std::array<std::uint8_t, UsbAudio::MaxPayloadSize>& Payload)
    {
        std::array<std::uint8_t, UsbAudio::HeaderSize> HeaderBytes{};
        if (!ReadExact(HeaderBytes.data(), HeaderBytes.size()))
        {
            return false;
        }
        while (!UsbAudio::DecodeHeader(HeaderBytes.data(), Header))
        {
            std::memmove(HeaderBytes.data(), HeaderBytes.data() + 1, HeaderBytes.size() - 1);
            if (!ReadExact(HeaderBytes.data() + HeaderBytes.size() - 1, 1))
            {
                return false;
            }
        }
        if (Header.PayloadSize > 0 && !ReadExact(Payload.data(), Header.PayloadSize))
        {
            return false;
        }
        while (UsbAudio::Crc32(Payload.data(), Header.PayloadSize) != Header.PayloadCrc)
        {
            CorruptPackets.fetch_add(1, std::memory_order_relaxed);
            if (!ReadExact(HeaderBytes.data(), HeaderBytes.size()))
            {
                return false;
            }
            while (!UsbAudio::DecodeHeader(HeaderBytes.data(), Header))
            {
                std::memmove(HeaderBytes.data(), HeaderBytes.data() + 1, HeaderBytes.size() - 1);
                if (!ReadExact(HeaderBytes.data() + HeaderBytes.size() - 1, 1))
                {
                    return false;
                }
            }
            if (Header.PayloadSize > 0 && !ReadExact(Payload.data(), Header.PayloadSize))
            {
                return false;
            }
        }
        return true;
    }

    bool WriteExact(const std::uint8_t* Data, std::size_t Size)
    {
        std::size_t Offset = 0;
        while (Offset < Size && Running.load(std::memory_order_acquire))
        {
            if (!WaitFor(POLLOUT))
            {
                return false;
            }
            const ssize_t Count = write(Descriptor, Data + Offset, Size - Offset);
            if (Count > 0)
            {
                Offset += static_cast<std::size_t>(Count);
            }
            else if (Count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                return false;
            }
        }
        return Offset == Size;
    }

    bool SendFeedback()
    {
        std::array<std::uint8_t, 16> Payload{};
        UsbAudio::WriteU32(Payload.data(), Ring.GetQueuedFrames());
        UsbAudio::WriteU32(Payload.data() + 4, LastSequence);
        UsbAudio::WriteU32(Payload.data() + 8, Underruns.load(std::memory_order_relaxed));
        const std::uint32_t ErrorCount = CorruptPackets.load(std::memory_order_relaxed) +
            LostPackets.load(std::memory_order_relaxed) + DroppedPackets.load(std::memory_order_relaxed);
        UsbAudio::WriteU32(Payload.data() + 12, ErrorCount);
        std::array<std::uint8_t, UsbAudio::HeaderSize> HeaderBytes{};
        const UsbAudio::PacketHeader Header{ UsbAudio::FeedbackPacketType, FeedbackSequence++, GetMonotonicNs(),
            static_cast<std::uint16_t>(Payload.size()), UsbAudio::Crc32(Payload.data(), Payload.size()) };
        UsbAudio::EncodeHeader(HeaderBytes.data(), Header);
        return WriteExact(HeaderBytes.data(), HeaderBytes.size()) && WriteExact(Payload.data(), Payload.size());
    }

    void RunTransport()
    {
        std::array<std::int16_t, 5760 * UsbAudio::Channels> Decoded{};
        auto NextFeedback = std::chrono::steady_clock::now();
        while (Running.load(std::memory_order_acquire))
        {
            UsbAudio::PacketHeader Header{};
            std::array<std::uint8_t, UsbAudio::MaxPayloadSize> Payload{};
            if (!ReadPacket(Header, Payload))
            {
                if (Running.load(std::memory_order_acquire))
                {
                    Running.store(false, std::memory_order_release);
                }
                break;
            }
            if (Header.Type == UsbAudio::AudioPacketType)
            {
                if (HaveSequence && Header.Sequence != ExpectedSequence)
                {
                    const std::uint32_t MissingPackets = Header.Sequence - ExpectedSequence;
                    LostPackets.fetch_add(MissingPackets, std::memory_order_relaxed);
                    const std::uint32_t ConcealmentFrames = std::min<std::uint32_t>(MissingPackets, 3);
                    for (std::uint32_t Missing = 0; Missing < ConcealmentFrames; ++Missing)
                    {
                        const int ConcealedFrames = opus_decode(Decoder, nullptr, 0, Decoded.data(),
                            static_cast<int>(UsbAudio::AudioFrameSamples), 0);
                        if (ConcealedFrames > 0)
                        {
                            Ring.Write(Decoded.data(), static_cast<std::size_t>(ConcealedFrames));
                        }
                    }
                }
                ExpectedSequence = Header.Sequence + 1;
                HaveSequence = true;
                LastSequence = Header.Sequence;
                const int Frames = opus_decode(Decoder, Payload.data(), Header.PayloadSize, Decoded.data(), 5760, 0);
                if (Frames < 0)
                {
                    CorruptPackets.fetch_add(1, std::memory_order_relaxed);
                    const int ConcealedFrames = opus_decode(Decoder, nullptr, 0, Decoded.data(),
                        static_cast<int>(UsbAudio::AudioFrameSamples), 0);
                    if (ConcealedFrames > 0)
                    {
                        Ring.Write(Decoded.data(), static_cast<std::size_t>(ConcealedFrames));
                    }
                }
                else if (Frames > 0 && !Ring.Write(Decoded.data(), static_cast<std::size_t>(Frames)))
                {
                    DroppedPackets.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const auto Now = std::chrono::steady_clock::now();
            if (Now >= NextFeedback)
            {
                if (!SendFeedback())
                {
                    Running.store(false, std::memory_order_release);
                    break;
                }
                NextFeedback = Now + std::chrono::milliseconds(250);
            }
        }
    }

    static std::uint64_t GetMonotonicNs()
    {
        timespec Value{};
        clock_gettime(CLOCK_MONOTONIC, &Value);
        return static_cast<std::uint64_t>(Value.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(Value.tv_nsec);
    }

    static constexpr std::uint64_t StartupFrames = 1920;
    static constexpr double TargetFrames = 2400.0;
    int Descriptor = -1;
    OpusDecoder* Decoder = nullptr;
    AAudioStream* Stream = nullptr;
    PcmRing Ring;
    std::atomic<bool> Running{false};
    std::atomic<std::uint32_t> Underruns{0};
    std::atomic<std::uint32_t> CorruptPackets{0};
    std::atomic<std::uint32_t> LostPackets{0};
    std::atomic<std::uint32_t> DroppedPackets{0};
    std::thread TransportThread;
    double ReadPosition = 0.0;
    bool PlaybackStarted = false;
    std::uint32_t LastSequence = 0;
    std::uint32_t ExpectedSequence = 0;
    std::uint32_t FeedbackSequence = 0;
    bool HaveSequence = false;
};

std::mutex ReceiverMutex;
std::unique_ptr<Receiver> ActiveReceiver;

jboolean StartNative(JNIEnv*, jobject, jint Descriptor)
{
    std::lock_guard<std::mutex> Lock(ReceiverMutex);
    ActiveReceiver.reset();
    auto NewReceiver = std::make_unique<Receiver>(Descriptor);
    if (!NewReceiver->Start())
    {
        return JNI_FALSE;
    }
    ActiveReceiver = std::move(NewReceiver);
    return JNI_TRUE;
}

void StopNative(JNIEnv*, jobject)
{
    std::lock_guard<std::mutex> Lock(ReceiverMutex);
    ActiveReceiver.reset();
}

jboolean IsRunningNative(JNIEnv*, jobject)
{
    std::lock_guard<std::mutex> Lock(ReceiverMutex);
    return ActiveReceiver != nullptr && ActiveReceiver->IsRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* Vm, void*)
{
    JNIEnv* Environment = nullptr;
    if (Vm->GetEnv(reinterpret_cast<void**>(&Environment), JNI_VERSION_1_6) != JNI_OK)
    {
        return JNI_ERR;
    }
    jclass ServiceClass = Environment->FindClass("com/example/usbaudio/AudioService");
    if (ServiceClass == nullptr)
    {
        return JNI_ERR;
    }
    const JNINativeMethod Methods[] = {
        { const_cast<char*>("StartNative"), const_cast<char*>("(I)Z"), reinterpret_cast<void*>(StartNative) },
        { const_cast<char*>("StopNative"), const_cast<char*>("()V"), reinterpret_cast<void*>(StopNative) },
        { const_cast<char*>("IsRunningNative"), const_cast<char*>("()Z"), reinterpret_cast<void*>(IsRunningNative) }
    };
    if (Environment->RegisterNatives(ServiceClass, Methods, sizeof(Methods) / sizeof(Methods[0])) != JNI_OK)
    {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

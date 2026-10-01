#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

#include <aaudio/AAudio.h>
#include <android/performance_hint.h>
#include <jni.h>

#include "AudioFormat.h"

class PerformanceHint
{
public:
    PerformanceHint() = default;
    PerformanceHint(const PerformanceHint&) = delete;
    PerformanceHint& operator=(const PerformanceHint&) = delete;

    ~PerformanceHint()
    {
        Close();
    }

    bool Open(std::int32_t ThreadId, std::int64_t TargetDurationNanos)
    {
        Close();
        APerformanceHintManager* Manager = APerformanceHint_getManager();
        if (Manager == nullptr)
        {
            return false;
        }
        Session = APerformanceHint_createSession(Manager, &ThreadId, 1, TargetDurationNanos);
        return Session != nullptr;
    }

    void Report(std::int64_t ActualDurationNanos) const
    {
        if (Session != nullptr)
        {
            APerformanceHint_reportActualWorkDuration(Session, ActualDurationNanos);
        }
    }

    void Close()
    {
        if (Session != nullptr)
        {
            APerformanceHint_closeSession(Session);
            Session = nullptr;
        }
    }

private:
    APerformanceHintSession* Session = nullptr;
};

class PcmRing
{
public:
    static constexpr std::uint64_t CapacityFrames = 1024;

    bool Write(const std::int16_t* Samples, std::size_t Frames)
    {
        const std::uint64_t WriteFrame = WriteIndex.load(std::memory_order_relaxed);
        const std::uint64_t ReadFrame = ReadIndex.load(std::memory_order_acquire);
        const std::uint64_t QueuedFrames = std::min<std::uint64_t>(WriteFrame - ReadFrame, CapacityFrames);
        if (Frames > CapacityFrames - QueuedFrames)
        {
            return false;
        }
        for (std::size_t Frame = 0; Frame < Frames; ++Frame)
        {
            SamplesData[static_cast<std::size_t>((WriteFrame + Frame) & (CapacityFrames - 1))] = Samples[Frame];
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

    std::int16_t GetSample(std::uint64_t Index) const
    {
        return SamplesData[static_cast<std::size_t>(Index & (CapacityFrames - 1))];
    }

private:
    std::array<std::int16_t, CapacityFrames> SamplesData{};
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
        if (Flags < 0 || fcntl(Descriptor, F_SETFL, Flags | O_NONBLOCK) < 0 || !OpenAudioStream())
        {
            Stop();
            return false;
        }
        Running.store(true, std::memory_order_release);
        TransportThread = std::thread(&Receiver::RunTransport, this);
        if (!WriteReady())
        {
            Stop();
            return false;
        }
        ReadinessThread = std::thread(&Receiver::RunReadiness, this);
        return true;
    }

    void Stop()
    {
        Running.store(false, std::memory_order_release);
        if (TransportThread.joinable())
        {
            TransportThread.join();
        }
        if (ReadinessThread.joinable())
        {
            ReadinessThread.join();
        }
        if (Stream != nullptr)
        {
            AAudioStream_requestStop(Stream);
            AAudioStream_close(Stream);
            Stream = nullptr;
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
        AAudioStreamBuilder_setSharingMode(Builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
        AAudioStreamBuilder_setUsage(Builder, AAUDIO_USAGE_GAME);
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
            AAudioStream_setBufferSizeInFrames(Stream, std::max<std::int32_t>(BurstFrames, MinimumOutputBufferFrames));
        }
        return AAudioStream_requestStart(Stream) == AAUDIO_OK;
    }

    void Render(std::int16_t* Output, std::int32_t NumFrames)
    {
        const std::uint64_t WriteFrame = Ring.GetWriteIndex();
        std::uint64_t ReadFrame = Ring.GetReadIndex();
        if (!PlaybackStarted)
        {
            if (WriteFrame - ReadFrame < StartupFrames)
            {
                std::memset(Output, 0, static_cast<std::size_t>(NumFrames) * sizeof(*Output));
                return;
            }
            PlaybackStarted = true;
        }
        const std::uint64_t CallbackFrames = static_cast<std::uint64_t>(NumFrames);
        std::uint64_t StaleFrame = ReadFrame;
        std::int32_t FadeFrames = 0;
        if (WriteFrame - ReadFrame > CallbackFrames + TrimHighWaterFrames)
        {
            ReadFrame = WriteFrame - (CallbackFrames + TrimTargetFrames);
            FadeFrames = std::min<std::int32_t>(NumFrames, TrimFadeFrames);
        }
        for (std::int32_t Frame = 0; Frame < NumFrames; ++Frame)
        {
            const std::int32_t Current = TakeSample(ReadFrame, WriteFrame);
            if (Frame < FadeFrames)
            {
                const std::int32_t Stale = TakeSample(StaleFrame, WriteFrame);
                Output[Frame] = static_cast<std::int16_t>((Stale * (FadeFrames - Frame) + Current * Frame) / FadeFrames);
            }
            else
            {
                Output[Frame] = static_cast<std::int16_t>(Current);
            }
        }
        Ring.PublishReadIndex(ReadFrame);
    }

    std::int16_t TakeSample(std::uint64_t& Frame, std::uint64_t WriteFrame) const
    {
        if (Frame >= WriteFrame)
        {
            return 0;
        }
        return Ring.GetSample(Frame++);
    }

    bool WaitFor(short Events)
    {
        pollfd PollDescriptor{};
        PollDescriptor.fd = Descriptor;
        PollDescriptor.events = Events;
        while (Running.load(std::memory_order_acquire))
        {
            const int Result = poll(&PollDescriptor, 1, TransportPollTimeoutMs);
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

    bool ReadExact(std::uint8_t* Data, std::size_t Size, std::chrono::steady_clock::time_point& ReadyTime)
    {
        std::size_t Offset = 0;
        while (Offset < Size && Running.load(std::memory_order_acquire))
        {
            if (!WaitFor(POLLIN))
            {
                return false;
            }
            if (Offset == 0)
            {
                ReadyTime = std::chrono::steady_clock::now();
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

    bool WriteReady()
    {
        const std::uint8_t Ready = UsbAudio::ReceiverReady;
        while (Running.load(std::memory_order_acquire))
        {
            if (!WaitFor(POLLOUT))
            {
                return false;
            }
            const ssize_t Count = write(Descriptor, &Ready, sizeof(Ready));
            if (Count == sizeof(Ready))
            {
                return true;
            }
            if (Count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                return false;
            }
        }
        return false;
    }

    void RunTransport()
    {
        const std::int32_t ThreadId = static_cast<std::int32_t>(gettid());
        setpriority(PRIO_PROCESS, static_cast<id_t>(ThreadId), TransportNicePriority);
        PerformanceHint Hint;
        const bool HintActive = Hint.Open(ThreadId, TransportTargetWorkNanos);
        std::array<std::int16_t, UsbAudio::AudioFrameSamples> Samples;
        std::chrono::steady_clock::time_point ReadyTime = std::chrono::steady_clock::now();
        std::int64_t WorstWorkNanos = 0;
        std::uint32_t PacketsSinceReport = 0;
        while (Running.load(std::memory_order_acquire))
        {
            if (!ReadExact(reinterpret_cast<std::uint8_t*>(Samples.data()), sizeof(Samples), ReadyTime))
            {
                if (Running.load(std::memory_order_acquire))
                {
                    Running.store(false, std::memory_order_release);
                }
                break;
            }
            Ring.Write(Samples.data(), Samples.size());
            if (!HintActive)
            {
                continue;
            }
            const std::int64_t WorkNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ReadyTime).count();
            WorstWorkNanos = std::max(WorstWorkNanos, WorkNanos);
            if (++PacketsSinceReport >= TransportReportIntervalPackets)
            {
                Hint.Report(WorstWorkNanos);
                WorstWorkNanos = 0;
                PacketsSinceReport = 0;
            }
        }
    }

    void RunReadiness()
    {
        while (Running.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(ReadinessIntervalMs));
            if (!Running.load(std::memory_order_acquire))
            {
                return;
            }
            if (!WriteReady())
            {
                if (Running.load(std::memory_order_acquire))
                {
                    Running.store(false, std::memory_order_release);
                }
                return;
            }
        }
    }

    static constexpr std::int32_t MinimumOutputBufferFrames = 192;
    static constexpr int TransportPollTimeoutMs = 100;
    static constexpr int ReadinessIntervalMs = 500;
    static constexpr int TransportNicePriority = -19;
    static constexpr std::int64_t TransportTargetWorkNanos = 300000;
    static constexpr std::uint32_t TransportReportIntervalPackets = 8;
    static constexpr std::uint64_t StartupFrames = 240;
    static constexpr std::uint64_t TrimTargetFrames = StartupFrames;
    static constexpr std::uint64_t TrimHighWaterFrames = 480;
    static constexpr std::int32_t TrimFadeFrames = 48;
    static_assert(TrimHighWaterFrames + TrimFadeFrames < PcmRing::CapacityFrames);
    static_assert(TrimTargetFrames < TrimHighWaterFrames);
    int Descriptor = -1;
    AAudioStream* Stream = nullptr;
    PcmRing Ring;
    std::atomic<bool> Running{false};
    std::thread TransportThread;
    std::thread ReadinessThread;
    bool PlaybackStarted = false;
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

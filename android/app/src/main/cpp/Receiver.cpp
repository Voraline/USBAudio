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
#include <numbers>
#include <poll.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

#include <aaudio/AAudio.h>
#include <android/log.h>
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
    static constexpr std::uint64_t CapacityFrames = 2048;

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

class InterpolationFilter
{
public:
    static constexpr std::size_t Taps = 16;
    static constexpr std::size_t Phases = 256;
    static constexpr std::size_t HistoryFrames = Taps / 2 - 1;
    static constexpr std::size_t LookaheadFrames = Taps / 2;

    InterpolationFilter()
    {
        for (std::size_t Row = 0; Row <= Phases; ++Row)
        {
            const double Fraction = static_cast<double>(Row) / static_cast<double>(Phases);
            std::array<double, Taps> Weights{};
            double Sum = 0.0;
            for (std::size_t Tap = 0; Tap < Taps; ++Tap)
            {
                const double Distance = static_cast<double>(Tap) - static_cast<double>(HistoryFrames) - Fraction;
                Weights[Tap] = Sinc(Distance) * Window(Distance);
                Sum += Weights[Tap];
            }
            for (std::size_t Tap = 0; Tap < Taps; ++Tap)
            {
                Table[Row][Tap] = static_cast<float>(Weights[Tap] / Sum);
            }
        }
    }

    float Apply(const PcmRing& Ring, std::uint64_t Base, double Fraction) const
    {
        const double Scaled = Fraction * static_cast<double>(Phases);
        const std::size_t Row = std::min(static_cast<std::size_t>(Scaled), Phases - 1);
        const float Blend = static_cast<float>(Scaled - static_cast<double>(Row));
        const std::array<float, Taps>& Lower = Table[Row];
        const std::array<float, Taps>& Upper = Table[Row + 1];
        const std::uint64_t First = Base - HistoryFrames;
        float Sum = 0.0f;
        for (std::size_t Tap = 0; Tap < Taps; ++Tap)
        {
            const float Weight = Lower[Tap] + (Upper[Tap] - Lower[Tap]) * Blend;
            Sum += Weight * static_cast<float>(Ring.GetSample(First + Tap));
        }
        return Sum;
    }

private:
    static double Sinc(double Value)
    {
        if (std::abs(Value) < 1.0e-12)
        {
            return 1.0;
        }
        const double Argument = std::numbers::pi * Value;
        return std::sin(Argument) / Argument;
    }

    static double Window(double Distance)
    {
        const double Angle = std::numbers::pi * Distance / static_cast<double>(LookaheadFrames);
        return 0.42 + 0.5 * std::cos(Angle) + 0.08 * std::cos(2.0 * Angle);
    }

    std::array<std::array<float, Taps>, Phases + 1> Table{};
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
    enum class PlaybackState
    {
        Priming,
        Playing
    };

    static aaudio_data_callback_result_t AudioCallback(AAudioStream*, void* UserData, void* AudioData, std::int32_t NumFrames)
    {
        auto* Self = static_cast<Receiver*>(UserData);
        Self->Render(static_cast<std::int16_t*>(AudioData), NumFrames);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static std::int16_t ToPcm16(float Value)
    {
        return static_cast<std::int16_t>(std::lrintf(std::clamp(Value, -32768.0f, 32767.0f)));
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
        const double TargetFrames = std::clamp(static_cast<double>(NumFrames) + TargetMarginFrames + ExtraTargetFrames, MinimumTargetFrames, MaximumTargetFrames);
        double Queued = static_cast<double>(WriteFrame) - Position;
        if (State == PlaybackState::Priming)
        {
            if (Queued < TargetFrames)
            {
                std::memset(Output, 0, static_cast<std::size_t>(NumFrames) * sizeof(*Output));
                LastValue = 0.0f;
                return;
            }
            State = PlaybackState::Playing;
            SmoothedError = 0.0;
            FadeInProgress = 0;
        }

        double StalePosition = Position;
        std::int32_t CrossfadeFrames = 0;
        if (Queued > TargetFrames + ResyncHighWaterFrames)
        {
            Position = static_cast<double>(WriteFrame) - TargetFrames;
            Queued = TargetFrames;
            SmoothedError = 0.0;
            CrossfadeFrames = std::min(NumFrames, ResyncFadeFrames);
            Resyncs.fetch_add(1, std::memory_order_relaxed);
        }
        UpdateController(Queued - TargetFrames, static_cast<double>(NumFrames) / SampleRateFrames);

        const double Step = 1.0 + Adjustment;
        std::int32_t Frame = 0;
        for (; Frame < NumFrames && HasLookahead(Position, WriteFrame); ++Frame)
        {
            float Value = Interpolate(Position);
            if (Frame < CrossfadeFrames)
            {
                const float Blend = static_cast<float>(Frame + 1) / static_cast<float>(CrossfadeFrames);
                const float Stale = Interpolate(StalePosition);
                Value = Stale + (Value - Stale) * Blend;
                StalePosition += Step;
            }
            if (FadeInProgress < StartFadeFrames)
            {
                Value *= static_cast<float>(FadeInProgress) / static_cast<float>(StartFadeFrames);
                ++FadeInProgress;
            }
            LastValue = Value;
            Output[Frame] = ToPcm16(Value);
            Position += Step;
        }
        if (Frame < NumFrames)
        {
            for (; Frame < NumFrames; ++Frame)
            {
                LastValue *= StarvationDecay;
                Output[Frame] = ToPcm16(LastValue);
            }
            State = PlaybackState::Priming;
            ExtraTargetFrames = std::min(ExtraTargetFrames + UnderrunTargetStepFrames, MaximumExtraTargetFrames);
            Underruns.fetch_add(1, std::memory_order_relaxed);
        }

        const std::uint64_t Integer = static_cast<std::uint64_t>(Position);
        Ring.PublishReadIndex(Integer > InterpolationFilter::HistoryFrames ? Integer - InterpolationFilter::HistoryFrames : 0);
        StatFillFrames.store(static_cast<std::int32_t>(Queued), std::memory_order_relaxed);
        StatTargetFrames.store(static_cast<std::int32_t>(TargetFrames), std::memory_order_relaxed);
        StatAdjustmentPpm.store(static_cast<std::int32_t>(Adjustment * 1.0e6), std::memory_order_relaxed);
    }

    void UpdateController(double ErrorFrames, double DeltaSeconds)
    {
        const double Smoothing = DeltaSeconds / (ErrorSmoothingSeconds + DeltaSeconds);
        SmoothedError += (ErrorFrames - SmoothedError) * Smoothing;
        Integral = std::clamp(Integral + SmoothedError * IntegralGain * DeltaSeconds, -MaximumAdjustment, MaximumAdjustment);
        Adjustment = std::clamp(SmoothedError * ProportionalGain + Integral, -MaximumAdjustment, MaximumAdjustment);
    }

    static bool HasLookahead(double SamplePosition, std::uint64_t WriteFrame)
    {
        return static_cast<std::uint64_t>(SamplePosition) + InterpolationFilter::LookaheadFrames < WriteFrame;
    }

    float Interpolate(double SamplePosition) const
    {
        const std::uint64_t Base = static_cast<std::uint64_t>(SamplePosition);
        return Filter.Apply(Ring, Base, SamplePosition - static_cast<double>(Base));
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

    bool ReadAvailable(std::uint8_t* Data, std::size_t Capacity, std::size_t& Received, std::chrono::steady_clock::time_point& ReadyTime)
    {
        while (Running.load(std::memory_order_acquire))
        {
            if (!WaitFor(POLLIN))
            {
                return false;
            }
            ReadyTime = std::chrono::steady_clock::now();
            const ssize_t Count = read(Descriptor, Data, Capacity);
            if (Count > 0)
            {
                Received = static_cast<std::size_t>(Count);
                return true;
            }
            if (Count == 0)
            {
                return false;
            }
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                return false;
            }
        }
        return false;
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
        std::array<std::int16_t, ReadRequestBytes / sizeof(std::int16_t) + 1> Samples{};
        auto* Bytes = reinterpret_cast<std::uint8_t*>(Samples.data());
        std::size_t CarryBytes = 0;
        std::chrono::steady_clock::time_point ReadyTime = std::chrono::steady_clock::now();
        std::int64_t WorstWorkNanos = 0;
        std::uint32_t PacketsSinceReport = 0;
        while (Running.load(std::memory_order_acquire))
        {
            std::size_t Received = 0;
            if (!ReadAvailable(Bytes + CarryBytes, ReadRequestBytes, Received, ReadyTime))
            {
                if (Running.load(std::memory_order_acquire))
                {
                    Running.store(false, std::memory_order_release);
                }
                break;
            }
            const std::size_t TotalBytes = CarryBytes + Received;
            const std::size_t WholeSamples = TotalBytes / sizeof(std::int16_t);
            if (WholeSamples > 0 && !Ring.Write(Samples.data(), WholeSamples))
            {
                DroppedChunks.fetch_add(1, std::memory_order_relaxed);
            }
            CarryBytes = TotalBytes - WholeSamples * sizeof(std::int16_t);
            if (CarryBytes > 0)
            {
                Bytes[0] = Bytes[WholeSamples * sizeof(std::int16_t)];
            }
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
        std::uint32_t Ticks = 0;
        std::uint32_t LoggedUnderruns = 0;
        std::uint32_t LoggedResyncs = 0;
        std::uint32_t LoggedDrops = 0;
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
            ++Ticks;
            const std::uint32_t CurrentUnderruns = Underruns.load(std::memory_order_relaxed);
            const std::uint32_t CurrentResyncs = Resyncs.load(std::memory_order_relaxed);
            const std::uint32_t CurrentDrops = DroppedChunks.load(std::memory_order_relaxed);
            const bool Changed = CurrentUnderruns != LoggedUnderruns || CurrentResyncs != LoggedResyncs || CurrentDrops != LoggedDrops;
            if (Changed || Ticks % StatsIntervalTicks == 0)
            {
                __android_log_print(ANDROID_LOG_INFO, LogTag, "fill=%d target=%d adjust=%dppm underruns=%u resyncs=%u dropped=%u",
                    StatFillFrames.load(std::memory_order_relaxed), StatTargetFrames.load(std::memory_order_relaxed),
                    StatAdjustmentPpm.load(std::memory_order_relaxed), CurrentUnderruns, CurrentResyncs, CurrentDrops);
                LoggedUnderruns = CurrentUnderruns;
                LoggedResyncs = CurrentResyncs;
                LoggedDrops = CurrentDrops;
            }
        }
    }

    static constexpr const char* LogTag = "UsbAudio";
    static constexpr double SampleRateFrames = UsbAudio::SampleRate;
    static constexpr std::int32_t MinimumOutputBufferFrames = 192;
    static constexpr int TransportPollTimeoutMs = 100;
    static constexpr int ReadinessIntervalMs = 500;
    static constexpr int TransportNicePriority = -19;
    static constexpr std::int64_t TransportTargetWorkNanos = 300000;
    static constexpr std::uint32_t TransportReportIntervalPackets = 8;
    static constexpr std::uint32_t StatsIntervalTicks = 20;
    static constexpr std::size_t ReadRequestBytes = 4096;
    static constexpr double MinimumTargetFrames = 240.0;
    static constexpr double MaximumTargetFrames = 1024.0;
    static constexpr double TargetMarginFrames = 128.0;
    static constexpr double UnderrunTargetStepFrames = 32.0;
    static constexpr double MaximumExtraTargetFrames = 512.0;
    static constexpr double ResyncHighWaterFrames = 960.0;
    static constexpr std::int32_t ResyncFadeFrames = 48;
    static constexpr std::int32_t StartFadeFrames = 48;
    static constexpr float StarvationDecay = 0.9f;
    static constexpr double ErrorSmoothingSeconds = 0.25;
    static constexpr double ProportionalGain = 1.25e-5;
    static constexpr double IntegralGain = 1.875e-6;
    static constexpr double MaximumAdjustment = 0.002;
    static_assert(UsbAudio::MaxPacketSamples * sizeof(std::int16_t) <= ReadRequestBytes);
    static_assert(MinimumTargetFrames > static_cast<double>(InterpolationFilter::LookaheadFrames));
    static_assert(MaximumTargetFrames + ResyncHighWaterFrames + static_cast<double>(InterpolationFilter::Taps) < static_cast<double>(PcmRing::CapacityFrames));

    int Descriptor = -1;
    AAudioStream* Stream = nullptr;
    PcmRing Ring;
    InterpolationFilter Filter;
    std::atomic<bool> Running{false};
    std::thread TransportThread;
    std::thread ReadinessThread;
    PlaybackState State = PlaybackState::Priming;
    double Position = 0.0;
    double SmoothedError = 0.0;
    double Integral = 0.0;
    double Adjustment = 0.0;
    double ExtraTargetFrames = 0.0;
    float LastValue = 0.0f;
    std::int32_t FadeInProgress = StartFadeFrames;
    std::atomic<std::uint32_t> Underruns{0};
    std::atomic<std::uint32_t> Resyncs{0};
    std::atomic<std::uint32_t> DroppedChunks{0};
    std::atomic<std::int32_t> StatFillFrames{0};
    std::atomic<std::int32_t> StatTargetFrames{0};
    std::atomic<std::int32_t> StatAdjustmentPpm{0};
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

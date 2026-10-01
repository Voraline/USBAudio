#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

template <typename RingType>
class DriftReader
{
public:
    DriftReader(RingType& RingRef, std::uint64_t PrimeFramesValue)
        : Ring(RingRef), PrimeFrames(PrimeFramesValue)
    {
    }

    void Render(std::int16_t* Output, std::int32_t NumFrames)
    {
        const std::uint64_t WriteFrame = Ring.GetWriteIndex();

        if (State == Mode::Priming)
        {
            const std::uint64_t ReadFrame = Ring.GetReadIndex();
            if (WriteFrame - ReadFrame < PrimeFrames)
            {
                std::memset(Output, 0, static_cast<std::size_t>(NumFrames) * sizeof(*Output));
                return;
            }
            Position = static_cast<double>(ReadFrame);
            FadeInRemaining = FadeFrames;
            Smoothed = static_cast<double>(WriteFrame - ReadFrame);
            AcquireCount = 0;
            AcquireSum = 0.0;
            State = Mode::Running;
        }

        const double Fill = static_cast<double>(WriteFrame) - Position;
        double Step = 1.0;
        std::int32_t TrimFade = 0;
        double StalePosition = Position;

        if (!SetpointKnown)
        {
            AcquireSum += Fill;
            if (++AcquireCount >= AcquireCallbacks)
            {
                Setpoint = AcquireSum / static_cast<double>(AcquireCount);
                Smoothed = Setpoint;
                SetpointKnown = true;
            }
        }
        else
        {
            if (Fill > Setpoint + TrimOverFrames)
            {
                Position = static_cast<double>(WriteFrame) - Setpoint;
                TrimFade = std::min<std::int32_t>(NumFrames, FadeFrames);
                TrimCount.fetch_add(1, std::memory_order_relaxed);
            }
            Smoothed += SmoothingAlpha * (Fill - Smoothed);
            const double Adjust = std::clamp(Gain * (Smoothed - Setpoint), -MaxAdjust, MaxAdjust);
            Step = 1.0 + Adjust;
        }

        for (std::int32_t Frame = 0; Frame < NumFrames; ++Frame)
        {
            if (Position + 1.0 >= static_cast<double>(WriteFrame))
            {
                const std::int32_t Remaining = NumFrames - Frame;
                const std::int32_t Fade = std::min<std::int32_t>(Remaining, FadeFrames);
                for (std::int32_t Index = 0; Index < Remaining; ++Index)
                {
                    Output[Frame + Index] = Index < Fade
                        ? static_cast<std::int16_t>((static_cast<std::int32_t>(Held) * (Fade - Index)) / Fade)
                        : 0;
                }
                Held = 0;
                UnderrunCount.fetch_add(1, std::memory_order_relaxed);
                State = Mode::Priming;
                break;
            }

            float Value = Interpolate(Position);
            if (Frame < TrimFade)
            {
                const float Mix = static_cast<float>(Frame) / static_cast<float>(TrimFade);
                const float Stale = (StalePosition + 1.0 < static_cast<double>(WriteFrame)) ? Interpolate(StalePosition) : Value;
                Value = Stale * (1.0f - Mix) + Value * Mix;
                StalePosition += Step;
            }
            if (FadeInRemaining > 0)
            {
                Value *= static_cast<float>(FadeFrames - FadeInRemaining) / static_cast<float>(FadeFrames);
                --FadeInRemaining;
            }
            Held = static_cast<std::int16_t>(std::clamp(std::lrintf(Value), -32768L, 32767L));
            Output[Frame] = Held;
            Position += Step;
        }

        std::uint64_t NewRead = static_cast<std::uint64_t>(Position);
        NewRead = std::min(NewRead, WriteFrame);
        Ring.PublishReadIndex(NewRead);
    }

    std::uint32_t Underruns() const { return UnderrunCount.load(std::memory_order_relaxed); }
    std::uint32_t Trims() const { return TrimCount.load(std::memory_order_relaxed); }
    double CurrentFill(std::uint64_t WriteFrame) const { return static_cast<double>(WriteFrame) - Position; }
    double GetSetpoint() const { return Setpoint; }

private:
    enum class Mode { Priming, Running };

    float Interpolate(double SamplePosition) const
    {
        const std::uint64_t Index = static_cast<std::uint64_t>(SamplePosition);
        const float Fraction = static_cast<float>(SamplePosition - static_cast<double>(Index));
        const float A = static_cast<float>(Ring.GetSample(Index));
        const float B = static_cast<float>(Ring.GetSample(Index + 1));
        return A + (B - A) * Fraction;
    }

    static constexpr double Gain = 1.0e-5;
    static constexpr double MaxAdjust = 1.0e-3;
    static constexpr double SmoothingAlpha = 1.0 / 128.0;
    static constexpr double TrimOverFrames = 240.0;
    static constexpr std::int32_t FadeFrames = 48;
    static constexpr std::uint32_t AcquireCallbacks = 256;

    RingType& Ring;
    std::uint64_t PrimeFrames;
    Mode State = Mode::Priming;
    double Position = 0.0;
    double Smoothed = 0.0;
    double Setpoint = 0.0;
    double AcquireSum = 0.0;
    std::uint32_t AcquireCount = 0;
    bool SetpointKnown = false;
    std::int32_t FadeInRemaining = 0;
    std::int16_t Held = 0;
    std::atomic<std::uint32_t> UnderrunCount{0};
    std::atomic<std::uint32_t> TrimCount{0};
};

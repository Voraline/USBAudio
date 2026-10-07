#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

class PolyphaseResampler
{
public:
    static constexpr std::size_t Taps = 16;
    static constexpr std::size_t Phases = 256;
    static constexpr std::size_t HistoryFrames = Taps / 2 - 1;

    PolyphaseResampler()
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

    void Reset()
    {
        History.fill(0.0f);
        Head = 0;
        Phase = 0.0;
    }

    template <typename OutputFn>
    void Push(float Sample, double Step, OutputFn&& Emit)
    {
        History[Head] = Sample;
        Head = (Head + 1) % Taps;

        while (Phase <= 1.0)
        {
            const double Scaled = Phase * static_cast<double>(Phases);
            const std::size_t Row = static_cast<std::size_t>(Scaled);
            const float Blend = static_cast<float>(Scaled - static_cast<double>(Row));
            const std::array<float, Taps>& Lower = Table[Row];
            const std::array<float, Taps>& Upper = Table[std::min(Row + 1, Phases)];

            float Gathered[Taps];
            for (std::size_t Tap = 0; Tap < Taps; ++Tap)
            {
                Gathered[Tap] = History[(Head + Tap) % Taps];
            }
            float Sum = 0.0f;
            for (std::size_t Tap = 0; Tap < Taps; ++Tap)
            {
                Sum += (Lower[Tap] + (Upper[Tap] - Lower[Tap]) * Blend) * Gathered[Tap];
            }

            Emit(Sum);
            Phase += Step;
        }
        Phase -= 1.0;
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
        const double Angle = std::numbers::pi * Distance / static_cast<double>(Taps / 2);
        return 0.42 + 0.5 * std::cos(Angle) + 0.08 * std::cos(2.0 * Angle);
    }

    std::array<std::array<float, Taps>, Phases + 1> Table{};
    std::array<float, Taps> History{};
    std::size_t Head = 0;
    double Phase = 0.0;
};

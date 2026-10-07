#pragma once

#include <cstdint>

class TpdfDither
{
public:
    float Next()
    {
        return NextUniform() + NextUniform();
    }

private:
    float NextUniform()
    {
        State ^= State << 13;
        State ^= State >> 17;
        State ^= State << 5;
        return static_cast<float>(State >> 8) * (1.0f / 16777216.0f) - 0.5f;
    }

    std::uint32_t State = 0x9E3779B9u;
};

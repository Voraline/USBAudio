#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "AudioFormat.h"

inline constexpr std::size_t AudioQueueCapacity = 64;

using AudioPacket = std::array<std::int16_t, UsbAudio::AudioFrameSamples>;

template <typename T, std::size_t Capacity>
class SpscQueue
{
public:
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);

    bool Push(const T& Value)
    {
        const std::size_t Write = WriteIndex.load(std::memory_order_relaxed);
        const std::size_t Next = (Write + 1) & (Capacity - 1);
        if (Next == ReadIndex.load(std::memory_order_acquire))
        {
            return false;
        }
        Items[Write] = Value;
        WriteIndex.store(Next, std::memory_order_release);
        return true;
    }

    bool Pop(T& Value)
    {
        const std::size_t Read = ReadIndex.load(std::memory_order_relaxed);
        if (Read == WriteIndex.load(std::memory_order_acquire))
        {
            return false;
        }
        Value = Items[Read];
        ReadIndex.store((Read + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> Items{};
    alignas(64) std::atomic<std::size_t> WriteIndex{0};
    alignas(64) std::atomic<std::size_t> ReadIndex{0};
};

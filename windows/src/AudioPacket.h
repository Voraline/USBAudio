#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Protocol.h"

struct AudioPacket
{
    std::uint32_t Sequence = 0;
    std::uint64_t TimestampNs = 0;
    std::uint16_t PayloadSize = 0;
    std::array<std::uint8_t, UsbAudio::MaxPayloadSize> Payload{};
};

template <typename T, std::size_t Capacity>
class SpscQueue
{
public:
    bool Push(const T& Value)
    {
        const std::size_t Write = WriteIndex.load(std::memory_order_relaxed);
        const std::size_t Next = (Write + 1) % Capacity;
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
        ReadIndex.store((Read + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> Items{};
    std::atomic<std::size_t> WriteIndex{0};
    std::atomic<std::size_t> ReadIndex{0};
};

#pragma once

#include <cstddef>
#include <cstdint>

namespace UsbAudio
{
    inline constexpr std::uint32_t Magic = 0x41425355;
    inline constexpr std::uint8_t Version = 1;
    inline constexpr std::uint8_t AudioPacketType = 1;
    inline constexpr std::uint8_t FeedbackPacketType = 2;
    inline constexpr std::size_t HeaderSize = 28;
    inline constexpr std::size_t MaxPayloadSize = 1275;
    inline constexpr std::size_t AudioFrameSamples = 960;
    inline constexpr std::uint32_t SampleRate = 48000;
    inline constexpr std::uint16_t Channels = 2;

    struct PacketHeader
    {
        std::uint8_t Type;
        std::uint32_t Sequence;
        std::uint64_t TimestampNs;
        std::uint16_t PayloadSize;
        std::uint32_t PayloadCrc;
    };

    inline std::uint16_t ReadU16(const std::uint8_t* Data)
    {
        return static_cast<std::uint16_t>(Data[0]) | static_cast<std::uint16_t>(Data[1] << 8);
    }

    inline std::uint32_t ReadU32(const std::uint8_t* Data)
    {
        return static_cast<std::uint32_t>(Data[0]) |
            (static_cast<std::uint32_t>(Data[1]) << 8) |
            (static_cast<std::uint32_t>(Data[2]) << 16) |
            (static_cast<std::uint32_t>(Data[3]) << 24);
    }

    inline std::uint64_t ReadU64(const std::uint8_t* Data)
    {
        return static_cast<std::uint64_t>(ReadU32(Data)) |
            (static_cast<std::uint64_t>(ReadU32(Data + 4)) << 32);
    }

    inline void WriteU16(std::uint8_t* Data, std::uint16_t Value)
    {
        Data[0] = static_cast<std::uint8_t>(Value);
        Data[1] = static_cast<std::uint8_t>(Value >> 8);
    }

    inline void WriteU32(std::uint8_t* Data, std::uint32_t Value)
    {
        Data[0] = static_cast<std::uint8_t>(Value);
        Data[1] = static_cast<std::uint8_t>(Value >> 8);
        Data[2] = static_cast<std::uint8_t>(Value >> 16);
        Data[3] = static_cast<std::uint8_t>(Value >> 24);
    }

    inline void WriteU64(std::uint8_t* Data, std::uint64_t Value)
    {
        WriteU32(Data, static_cast<std::uint32_t>(Value));
        WriteU32(Data + 4, static_cast<std::uint32_t>(Value >> 32));
    }

    inline std::uint32_t Crc32(const std::uint8_t* Data, std::size_t Size)
    {
        std::uint32_t Crc = 0xFFFFFFFF;
        for (std::size_t Index = 0; Index < Size; ++Index)
        {
            Crc ^= Data[Index];
            for (int Bit = 0; Bit < 8; ++Bit)
            {
                Crc = (Crc >> 1) ^ (0xEDB88320 & (0U - (Crc & 1U)));
            }
        }
        return ~Crc;
    }

    inline void EncodeHeader(std::uint8_t* Data, const PacketHeader& Header)
    {
        WriteU32(Data, Magic);
        Data[4] = Version;
        Data[5] = Header.Type;
        WriteU16(Data + 6, static_cast<std::uint16_t>(HeaderSize));
        WriteU32(Data + 8, Header.Sequence);
        WriteU64(Data + 12, Header.TimestampNs);
        WriteU16(Data + 20, Header.PayloadSize);
        WriteU16(Data + 22, 0);
        WriteU32(Data + 24, Header.PayloadCrc);
    }

    inline bool DecodeHeader(const std::uint8_t* Data, PacketHeader& Header)
    {
        if (ReadU32(Data) != Magic || Data[4] != Version || ReadU16(Data + 6) != HeaderSize)
        {
            return false;
        }
        Header.Type = Data[5];
        Header.Sequence = ReadU32(Data + 8);
        Header.TimestampNs = ReadU64(Data + 12);
        Header.PayloadSize = ReadU16(Data + 20);
        Header.PayloadCrc = ReadU32(Data + 24);
        return Header.PayloadSize <= MaxPayloadSize;
    }
}

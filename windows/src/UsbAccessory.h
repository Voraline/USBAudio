#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include <windows.h>
#include <winusb.h>

#include "AudioFormat.h"

class UsbAccessoryLink
{
public:
    UsbAccessoryLink() = default;
    ~UsbAccessoryLink();
    UsbAccessoryLink(const UsbAccessoryLink&) = delete;
    UsbAccessoryLink& operator=(const UsbAccessoryLink&) = delete;

    bool EnterAccessoryMode(std::uint16_t VendorId, std::uint16_t ProductId);
    bool TryOpenAccessory();
    bool WaitForAccessory(std::atomic<bool>& Running, std::uint32_t TimeoutMilliseconds);
    bool WaitForReceiver(std::atomic<bool>& Running, std::uint32_t TimeoutMilliseconds = 0);
    bool WriteAudio(const std::int16_t* Samples, std::size_t Count);
    void CancelTransfers();
    void Close();

private:
    struct WriteSlot
    {
        OVERLAPPED Overlapped{};
        HANDLE Event = nullptr;
        ULONG Size = 0;
        bool Pending = false;
        std::array<std::uint8_t, UsbAudio::MaxPacketSamples * sizeof(std::int16_t)> Buffer{};
    };

    bool OpenMatchingDevice(std::uint16_t VendorId, std::uint16_t ProductId, UCHAR RequiredInterfaceNumber = 0xFF);
    bool OpenPath(const std::wstring& DevicePath);
    bool SelectBulkPipes();
    bool CreateWriteSlots();
    bool WriteBytes(const std::uint8_t* Data, std::size_t Size);
    bool CompleteWrite(WriteSlot& Slot, DWORD TimeoutMilliseconds);

    static constexpr std::size_t WriteSlotCount = 4;
    static constexpr DWORD PipeTimeoutMs = 350;

    HANDLE DeviceHandle = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE InterfaceHandle = nullptr;
    UCHAR OutPipe = 0;
    UCHAR InPipe = 0;
    std::array<WriteSlot, WriteSlotCount> WriteSlots{};
    std::size_t NextWriteSlot = 0;
};

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
    bool WriteAudio(const std::int16_t* Samples);
    void CancelTransfers();
    void Close();

private:
    bool OpenMatchingDevice(std::uint16_t VendorId, std::uint16_t ProductId, UCHAR RequiredInterfaceNumber = 0xFF);
    bool OpenPath(const std::wstring& DevicePath);
    bool SelectBulkPipes();
    bool WriteBytes(const std::uint8_t* Data, std::size_t Size);
    bool CompletePendingWrite(DWORD TimeoutMilliseconds);

    HANDLE DeviceHandle = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE InterfaceHandle = nullptr;
    UCHAR OutPipe = 0;
    UCHAR InPipe = 0;
    HANDLE WriteEvent = nullptr;
    OVERLAPPED WriteOverlapped{};
    bool WritePending = false;
    ULONG PendingWriteSize = 0;
    std::array<std::uint8_t, UsbAudio::AudioFrameSamples * sizeof(std::int16_t)> WriteBuffer{};
    static constexpr DWORD PipeTimeoutMs = 350;
};

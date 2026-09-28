#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <windows.h>
#include <winusb.h>

class UsbAccessoryLink
{
public:
    UsbAccessoryLink() = default;
    ~UsbAccessoryLink();
    UsbAccessoryLink(const UsbAccessoryLink&) = delete;
    UsbAccessoryLink& operator=(const UsbAccessoryLink&) = delete;

    bool EnterAccessoryMode(std::uint16_t VendorId, std::uint16_t ProductId);
    bool TryOpenAccessory();
    bool WaitForAccessory(std::atomic<bool>& Running);
    bool WriteAudio(std::uint32_t Sequence, std::uint64_t TimestampNs, const std::uint8_t* Payload, std::uint16_t PayloadSize);
    bool ReadFeedback(std::atomic<bool>& Running);
    bool IsOpen() const;
    void CancelTransfers();
    void Close();

private:
    bool OpenMatchingDevice(std::uint16_t VendorId, std::uint16_t ProductId);
    bool OpenPath(const std::wstring& DevicePath);
    bool SelectBulkPipes();
    bool WriteBytes(const std::uint8_t* Data, std::size_t Size);

    HANDLE DeviceHandle = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE InterfaceHandle = nullptr;
    UCHAR OutPipe = 0;
    UCHAR InPipe = 0;
};

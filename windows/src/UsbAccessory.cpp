#include "UsbAccessory.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iostream>
#include <thread>
#include <vector>

#include <setupapi.h>

#include "Protocol.h"

namespace
{
    constexpr GUID UsbAudioInterfaceGuid = { 0xA6D1C905, 0x76DA, 0x4DBB, { 0x8C, 0x82, 0x91, 0x61, 0x54, 0x86, 0xC8, 0xB5 } };
    constexpr std::uint16_t GoogleVendorId = 0x18D1;
    constexpr std::uint16_t AccessoryProductId = 0x2D00;
    constexpr std::uint16_t AccessoryAdbProductId = 0x2D01;

    std::wstring MakeId(std::uint16_t VendorId, std::uint16_t ProductId)
    {
        wchar_t Buffer[32]{};
        swprintf_s(Buffer, L"VID_%04X&PID_%04X", VendorId, ProductId);
        return Buffer;
    }

    std::wstring Uppercase(std::wstring Value)
    {
        for (wchar_t& Character : Value)
        {
            Character = static_cast<wchar_t>(towupper(Character));
        }
        return Value;
    }

    bool ContainsId(const std::wstring& DevicePath, std::uint16_t VendorId, std::uint16_t ProductId)
    {
        return Uppercase(DevicePath).find(MakeId(VendorId, ProductId)) != std::wstring::npos;
    }

    bool IsTimeoutError(DWORD Error)
    {
        return Error == ERROR_SEM_TIMEOUT || Error == ERROR_TIMEOUT || Error == ERROR_OPERATION_ABORTED;
    }
}

UsbAccessoryLink::~UsbAccessoryLink()
{
    Close();
}

bool UsbAccessoryLink::EnterAccessoryMode(std::uint16_t VendorId, std::uint16_t ProductId)
{
    if (!OpenMatchingDevice(VendorId, ProductId))
    {
        std::wcerr << L"Could not open the requested phone USB interface through WinUSB.\n";
        return false;
    }

    WINUSB_SETUP_PACKET Setup{};
    Setup.RequestType = 0xC0;
    Setup.Request = 51;
    Setup.Length = sizeof(USHORT);
    USHORT Protocol = 0;
    ULONG Transferred = 0;
    if (!WinUsb_ControlTransfer(InterfaceHandle, Setup, reinterpret_cast<PUCHAR>(&Protocol), sizeof(Protocol), &Transferred, nullptr) || Transferred < sizeof(Protocol) || Protocol < 1)
    {
        std::wcerr << L"The selected USB device did not respond to the Android Open Accessory protocol request.\n";
        Close();
        return false;
    }

    const std::array<std::wstring, 6> Strings = { L"OpenAI", L"USB Audio Stream", L"USB Opus Audio", L"1.0", L"https://github.com/", L"USBAudio" };
    Setup = {};
    Setup.RequestType = 0x40;
    Setup.Request = 52;
    for (std::size_t Index = 0; Index < Strings.size(); ++Index)
    {
        const std::wstring& Value = Strings[Index];
        Setup.Value = 0;
        Setup.Index = static_cast<USHORT>(Index);
        Setup.Length = static_cast<USHORT>((Value.size() + 1) * sizeof(wchar_t));
        std::vector<UCHAR> Utf8;
        const int Utf8Size = WideCharToMultiByte(CP_UTF8, 0, Value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (Utf8Size <= 0)
        {
            Close();
            return false;
        }
        Utf8.resize(static_cast<std::size_t>(Utf8Size));
        WideCharToMultiByte(CP_UTF8, 0, Value.c_str(), -1, reinterpret_cast<char*>(Utf8.data()), Utf8Size, nullptr, nullptr);
        Setup.Length = static_cast<USHORT>(Utf8.size());
        if (!WinUsb_ControlTransfer(InterfaceHandle, Setup, Utf8.data(), static_cast<USHORT>(Utf8.size()), &Transferred, nullptr))
        {
            std::wcerr << L"Could not send the Android accessory identification string.\n";
            Close();
            return false;
        }
    }

    Setup = {};
    Setup.RequestType = 0x40;
    Setup.Request = 53;
    if (!WinUsb_ControlTransfer(InterfaceHandle, Setup, nullptr, 0, &Transferred, nullptr))
    {
        std::wcerr << L"Could not start Android Open Accessory mode.\n";
        Close();
        return false;
    }
    Close();
    return true;
}

bool UsbAccessoryLink::TryOpenAccessory()
{
    const std::array<std::uint16_t, 2> ProductIds = { AccessoryProductId, AccessoryAdbProductId };
    for (const std::uint16_t ProductId : ProductIds)
    {
        if (OpenMatchingDevice(GoogleVendorId, ProductId))
        {
            if (SelectBulkPipes())
            {
                std::wcout << L"USB accessory audio link is ready.\n";
                return true;
            }
            Close();
        }
    }
    return false;
}

bool UsbAccessoryLink::WaitForAccessory(std::atomic<bool>& Running)
{
    std::wcout << L"Waiting for the phone to reconnect in USB accessory mode...\n";
    while (Running.load(std::memory_order_acquire))
    {
        if (TryOpenAccessory())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    return false;
}

bool UsbAccessoryLink::OpenMatchingDevice(std::uint16_t VendorId, std::uint16_t ProductId)
{
    HDEVINFO DeviceInfo = SetupDiGetClassDevsW(&UsbAudioInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (DeviceInfo == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    bool Opened = false;
    for (DWORD Index = 0; !Opened; ++Index)
    {
        SP_DEVICE_INTERFACE_DATA InterfaceData{};
        InterfaceData.cbSize = sizeof(InterfaceData);
        if (!SetupDiEnumDeviceInterfaces(DeviceInfo, nullptr, &UsbAudioInterfaceGuid, Index, &InterfaceData))
        {
            break;
        }
        DWORD RequiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(DeviceInfo, &InterfaceData, nullptr, 0, &RequiredSize, nullptr);
        if (RequiredSize == 0)
        {
            continue;
        }
        std::vector<std::uint8_t> DetailBuffer(RequiredSize);
        auto* Detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(DetailBuffer.data());
        Detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(DeviceInfo, &InterfaceData, Detail, RequiredSize, nullptr, nullptr))
        {
            continue;
        }
        if (ContainsId(Detail->DevicePath, VendorId, ProductId))
        {
            Opened = OpenPath(Detail->DevicePath);
        }
    }
    SetupDiDestroyDeviceInfoList(DeviceInfo);
    return Opened;
}

bool UsbAccessoryLink::OpenPath(const std::wstring& DevicePath)
{
    Close();
    DeviceHandle = CreateFileW(DevicePath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (DeviceHandle == INVALID_HANDLE_VALUE || !WinUsb_Initialize(DeviceHandle, &InterfaceHandle))
    {
        Close();
        return false;
    }
    return true;
}

bool UsbAccessoryLink::SelectBulkPipes()
{
    USB_INTERFACE_DESCRIPTOR Descriptor{};
    if (!WinUsb_QueryInterfaceSettings(InterfaceHandle, 0, &Descriptor))
    {
        return false;
    }
    OutPipe = 0;
    InPipe = 0;
    for (UCHAR Index = 0; Index < Descriptor.bNumEndpoints; ++Index)
    {
        WINUSB_PIPE_INFORMATION Pipe{};
        if (!WinUsb_QueryPipe(InterfaceHandle, 0, Index, &Pipe) || Pipe.PipeType != UsbdPipeTypeBulk)
        {
            continue;
        }
        if ((Pipe.PipeId & 0x80) != 0)
        {
            InPipe = Pipe.PipeId;
        }
        else
        {
            OutPipe = Pipe.PipeId;
        }
    }
    if (OutPipe == 0 || InPipe == 0)
    {
        return false;
    }
    ULONG TimeoutMs = 350;
    WinUsb_SetPipePolicy(InterfaceHandle, InPipe, PIPE_TRANSFER_TIMEOUT, sizeof(TimeoutMs), &TimeoutMs);
    WinUsb_SetPipePolicy(InterfaceHandle, OutPipe, PIPE_TRANSFER_TIMEOUT, sizeof(TimeoutMs), &TimeoutMs);
    return true;
}

bool UsbAccessoryLink::WriteBytes(const std::uint8_t* Data, std::size_t Size)
{
    std::size_t Offset = 0;
    while (Offset < Size)
    {
        ULONG Transferred = 0;
        const ULONG Requested = static_cast<ULONG>(Size - Offset);
        if (!WinUsb_WritePipe(InterfaceHandle, OutPipe, const_cast<PUCHAR>(Data + Offset), Requested, &Transferred, nullptr) || Transferred == 0)
        {
            return false;
        }
        Offset += Transferred;
    }
    return true;
}

bool UsbAccessoryLink::WriteAudio(std::uint32_t Sequence, std::uint64_t TimestampNs, const std::uint8_t* Payload, std::uint16_t PayloadSize)
{
    if (InterfaceHandle == nullptr || PayloadSize > UsbAudio::MaxPayloadSize)
    {
        return false;
    }
    std::array<std::uint8_t, UsbAudio::HeaderSize> HeaderData{};
    const UsbAudio::PacketHeader Header{ UsbAudio::AudioPacketType, Sequence, TimestampNs, PayloadSize, UsbAudio::Crc32(Payload, PayloadSize) };
    UsbAudio::EncodeHeader(HeaderData.data(), Header);
    return WriteBytes(HeaderData.data(), HeaderData.size()) && WriteBytes(Payload, PayloadSize);
}

bool UsbAccessoryLink::ReadFeedback(std::atomic<bool>& Running)
{
    std::array<std::uint8_t, 4096> Buffer{};
    std::size_t Buffered = 0;
    std::array<std::uint8_t, 512> ReadBuffer{};
    while (Running.load(std::memory_order_acquire) && InterfaceHandle != nullptr)
    {
        ULONG Transferred = 0;
        if (!WinUsb_ReadPipe(InterfaceHandle, InPipe, ReadBuffer.data(), static_cast<ULONG>(ReadBuffer.size()), &Transferred, nullptr))
        {
            const DWORD Error = GetLastError();
            if (IsTimeoutError(Error))
            {
                continue;
            }
            if (Running.load(std::memory_order_acquire))
            {
                std::wcerr << L"USB feedback endpoint disconnected.\n";
            }
            return false;
        }
        if (Transferred == 0)
        {
            continue;
        }
        if (Buffered + Transferred > Buffer.size())
        {
            Buffered = 0;
        }
        std::copy_n(ReadBuffer.data(), Transferred, Buffer.data() + Buffered);
        Buffered += Transferred;
        while (Buffered >= UsbAudio::HeaderSize)
        {
            UsbAudio::PacketHeader Header{};
            if (!UsbAudio::DecodeHeader(Buffer.data(), Header))
            {
                std::move(Buffer.begin() + 1, Buffer.begin() + Buffered, Buffer.begin());
                --Buffered;
                continue;
            }
            const std::size_t PacketSize = UsbAudio::HeaderSize + Header.PayloadSize;
            if (Buffered < PacketSize)
            {
                break;
            }
            const std::uint8_t* Payload = Buffer.data() + UsbAudio::HeaderSize;
            if (Header.Type == UsbAudio::FeedbackPacketType && Header.PayloadSize == 16 && UsbAudio::Crc32(Payload, Header.PayloadSize) == Header.PayloadCrc)
            {
                const std::uint32_t QueueFrames = UsbAudio::ReadU32(Payload);
                const std::uint32_t LastSequence = UsbAudio::ReadU32(Payload + 4);
                const std::uint32_t Underruns = UsbAudio::ReadU32(Payload + 8);
                static_cast<void>(LastSequence);
                static_cast<void>(Underruns);
                static_cast<void>(QueueFrames);
            }
            std::move(Buffer.begin() + PacketSize, Buffer.begin() + Buffered, Buffer.begin());
            Buffered -= PacketSize;
        }
    }
    return !Running.load(std::memory_order_acquire);
}

bool UsbAccessoryLink::IsOpen() const
{
    return InterfaceHandle != nullptr;
}

void UsbAccessoryLink::CancelTransfers()
{
    if (InterfaceHandle != nullptr)
    {
        if (InPipe != 0)
        {
            WinUsb_AbortPipe(InterfaceHandle, InPipe);
        }
        if (OutPipe != 0)
        {
            WinUsb_AbortPipe(InterfaceHandle, OutPipe);
        }
    }
}

void UsbAccessoryLink::Close()
{
    if (InterfaceHandle != nullptr)
    {
        CancelTransfers();
        WinUsb_Free(InterfaceHandle);
        InterfaceHandle = nullptr;
    }
    if (DeviceHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(DeviceHandle);
        DeviceHandle = INVALID_HANDLE_VALUE;
    }
    OutPipe = 0;
    InPipe = 0;
}

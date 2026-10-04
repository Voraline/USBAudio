#include "UsbAccessory.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <thread>
#include <vector>

#include <objbase.h>
#include <setupapi.h>

#include "AudioFormat.h"

namespace
{
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

    void AppendInterfaceGuids(HKEY RegistryKey, const wchar_t* ValueName, std::vector<GUID>& InterfaceGuids)
    {
        DWORD DataType = 0;
        DWORD DataSize = 0;
        if (RegQueryValueExW(RegistryKey, ValueName, nullptr, &DataType, nullptr, &DataSize) != ERROR_SUCCESS ||
            (DataType != REG_SZ && DataType != REG_MULTI_SZ) || DataSize < sizeof(wchar_t))
        {
            return;
        }

        std::vector<wchar_t> Data(DataSize / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(RegistryKey, ValueName, nullptr, &DataType, reinterpret_cast<LPBYTE>(Data.data()), &DataSize) != ERROR_SUCCESS)
        {
            return;
        }

        const std::size_t CharacterCount = DataSize / sizeof(wchar_t);
        for (std::size_t Offset = 0; Offset < CharacterCount;)
        {
            if (Data[Offset] == L'\0')
            {
                ++Offset;
                continue;
            }

            std::size_t End = Offset;
            while (End < CharacterCount && Data[End] != L'\0')
            {
                ++End;
            }

            const std::wstring Value(Data.data() + Offset, End - Offset);
            GUID InterfaceGuid{};
            if (SUCCEEDED(CLSIDFromString(Value.c_str(), &InterfaceGuid)) &&
                std::none_of(InterfaceGuids.begin(), InterfaceGuids.end(), [&InterfaceGuid](const GUID& ExistingGuid)
                {
                    return IsEqualGUID(ExistingGuid, InterfaceGuid) != FALSE;
                }))
            {
                InterfaceGuids.push_back(InterfaceGuid);
            }

            Offset = End + 1;
        }
    }

    std::vector<GUID> FindInterfaceGuids(std::uint16_t VendorId, std::uint16_t ProductId)
    {
        std::vector<GUID> InterfaceGuids;
        HDEVINFO DeviceInfo = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (DeviceInfo == INVALID_HANDLE_VALUE)
        {
            return InterfaceGuids;
        }

        for (DWORD Index = 0;; ++Index)
        {
            SP_DEVINFO_DATA DeviceData{};
            DeviceData.cbSize = sizeof(DeviceData);
            if (!SetupDiEnumDeviceInfo(DeviceInfo, Index, &DeviceData))
            {
                break;
            }

            DWORD RequiredSize = 0;
            SetupDiGetDeviceInstanceIdW(DeviceInfo, &DeviceData, nullptr, 0, &RequiredSize);
            if (RequiredSize == 0)
            {
                continue;
            }

            std::vector<wchar_t> InstanceId(RequiredSize + 1, L'\0');
            if (!SetupDiGetDeviceInstanceIdW(DeviceInfo, &DeviceData, InstanceId.data(), static_cast<DWORD>(InstanceId.size()), nullptr) ||
                !ContainsId(InstanceId.data(), VendorId, ProductId))
            {
                continue;
            }

            HKEY RegistryKey = SetupDiOpenDevRegKey(DeviceInfo, &DeviceData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
            if (RegistryKey == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            AppendInterfaceGuids(RegistryKey, L"DeviceInterfaceGUIDs", InterfaceGuids);
            AppendInterfaceGuids(RegistryKey, L"DeviceInterfaceGUID", InterfaceGuids);
            RegCloseKey(RegistryKey);
        }

        SetupDiDestroyDeviceInfoList(DeviceInfo);
        return InterfaceGuids;
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
        return false;
    }

    WINUSB_SETUP_PACKET Setup{};
    Setup.RequestType = 0xC0;
    Setup.Request = 51;
    Setup.Length = sizeof(USHORT);
    USHORT AccessoryProtocolVersion = 0;
    ULONG Transferred = 0;
    if (!WinUsb_ControlTransfer(InterfaceHandle, Setup, reinterpret_cast<PUCHAR>(&AccessoryProtocolVersion), sizeof(AccessoryProtocolVersion), &Transferred, nullptr) || Transferred < sizeof(AccessoryProtocolVersion) || AccessoryProtocolVersion < 1)
    {
        Close();
        return false;
    }

    const std::array<std::wstring, 6> Strings = { L"OpenAI", L"USB Audio Stream", L"USB PCM Audio", L"", L"https://github.com/", L"USBAudio" };
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
            Close();
            return false;
        }
    }

    Setup = {};
    Setup.RequestType = 0x40;
    Setup.Request = 53;
    if (!WinUsb_ControlTransfer(InterfaceHandle, Setup, nullptr, 0, &Transferred, nullptr))
    {
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
        if (OpenMatchingDevice(GoogleVendorId, ProductId, 0))
        {
            if (SelectBulkPipes())
            {
                return true;
            }
            Close();
        }
    }
    return false;
}

bool UsbAccessoryLink::WaitForAccessory(std::atomic<bool>& Running, std::uint32_t TimeoutMilliseconds)
{
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMilliseconds);
    while (Running.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < Deadline)
    {
        if (TryOpenAccessory())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool UsbAccessoryLink::WaitForReceiver(std::atomic<bool>& Running, std::uint32_t TimeoutMilliseconds)
{
    if (InterfaceHandle == nullptr || InPipe == 0)
    {
        return false;
    }
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMilliseconds);
    while (Running.load(std::memory_order_acquire) &&
        (TimeoutMilliseconds == 0 || std::chrono::steady_clock::now() < Deadline))
    {
        UCHAR Ready = 0;
        ULONG Transferred = 0;
        if (WinUsb_ReadPipe(InterfaceHandle, InPipe, &Ready, static_cast<ULONG>(sizeof(Ready)), &Transferred, nullptr))
        {
            if (Transferred == sizeof(Ready) && Ready == UsbAudio::ReceiverReady)
            {
                return true;
            }
            continue;
        }
        const DWORD Error = GetLastError();
        if (!IsTimeoutError(Error))
        {
            return false;
        }
    }
    return false;
}

bool UsbAccessoryLink::OpenMatchingDevice(std::uint16_t VendorId, std::uint16_t ProductId, UCHAR RequiredInterfaceNumber)
{
    const std::vector<GUID> InterfaceGuids = FindInterfaceGuids(VendorId, ProductId);
    for (const GUID& InterfaceGuid : InterfaceGuids)
    {
        HDEVINFO DeviceInfo = SetupDiGetClassDevsW(&InterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (DeviceInfo == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        bool Opened = false;
        for (DWORD Index = 0; !Opened; ++Index)
        {
            SP_DEVICE_INTERFACE_DATA InterfaceData{};
            InterfaceData.cbSize = sizeof(InterfaceData);
            if (!SetupDiEnumDeviceInterfaces(DeviceInfo, nullptr, &InterfaceGuid, Index, &InterfaceData))
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
                if (!OpenPath(Detail->DevicePath))
                {
                    continue;
                }

                if (RequiredInterfaceNumber != 0xFF)
                {
                    USB_INTERFACE_DESCRIPTOR Descriptor{};
                    if (!WinUsb_QueryInterfaceSettings(InterfaceHandle, 0, &Descriptor))
                    {
                        Close();
                        continue;
                    }
                    if (Descriptor.bInterfaceNumber != RequiredInterfaceNumber)
                    {
                        Close();
                        continue;
                    }
                }

                Opened = true;
            }
        }

        SetupDiDestroyDeviceInfoList(DeviceInfo);
        if (Opened)
        {
            return true;
        }
    }
    return false;
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
    if (!WinUsb_QueryInterfaceSettings(InterfaceHandle, 0, &Descriptor) || Descriptor.bInterfaceNumber != 0)
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
        if ((Pipe.PipeId & 0x80) == 0)
        {
            OutPipe = Pipe.PipeId;
        }
        else
        {
            InPipe = Pipe.PipeId;
        }
    }
    if (OutPipe == 0 || InPipe == 0)
    {
        return false;
    }
    ULONG TimeoutMs = 350;
    WinUsb_SetPipePolicy(InterfaceHandle, InPipe, PIPE_TRANSFER_TIMEOUT, sizeof(TimeoutMs), &TimeoutMs);
    UCHAR ShortPacketTerminate = TRUE;
    WinUsb_SetPipePolicy(InterfaceHandle, OutPipe, SHORT_PACKET_TERMINATE, sizeof(ShortPacketTerminate), &ShortPacketTerminate);
    UCHAR AutoSuspend = FALSE;
    WinUsb_SetPowerPolicy(InterfaceHandle, AUTO_SUSPEND, sizeof(AutoSuspend), &AutoSuspend);
    return CreateWriteSlots();
}

bool UsbAccessoryLink::CreateWriteSlots()
{
    for (WriteSlot& Slot : WriteSlots)
    {
        if (Slot.Event == nullptr)
        {
            Slot.Event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (Slot.Event == nullptr)
            {
                return false;
            }
        }
        Slot.Pending = false;
    }
    NextWriteSlot = 0;
    return true;
}

bool UsbAccessoryLink::CompleteWrite(WriteSlot& Slot, DWORD TimeoutMilliseconds)
{
    if (!Slot.Pending)
    {
        return true;
    }
    const DWORD WaitResult = WaitForSingleObject(Slot.Event, TimeoutMilliseconds);
    if (WaitResult != WAIT_OBJECT_0)
    {
        WinUsb_AbortPipe(InterfaceHandle, OutPipe);
        ULONG Discarded = 0;
        WinUsb_GetOverlappedResult(InterfaceHandle, &Slot.Overlapped, &Discarded, TRUE);
        Slot.Pending = false;
        return false;
    }
    ULONG Transferred = 0;
    const bool Completed = WinUsb_GetOverlappedResult(InterfaceHandle, &Slot.Overlapped, &Transferred, FALSE) != FALSE;
    Slot.Pending = false;
    return Completed && Transferred == Slot.Size;
}

bool UsbAccessoryLink::WriteBytes(const std::uint8_t* Data, std::size_t Size)
{
    WriteSlot& Slot = WriteSlots[NextWriteSlot];
    if (Size > Slot.Buffer.size() || !CompleteWrite(Slot, PipeTimeoutMs))
    {
        return false;
    }
    std::memcpy(Slot.Buffer.data(), Data, Size);
    Slot.Overlapped = {};
    Slot.Overlapped.hEvent = Slot.Event;
    Slot.Size = static_cast<ULONG>(Size);
    ULONG Transferred = 0;
    if (!WinUsb_WritePipe(InterfaceHandle, OutPipe, Slot.Buffer.data(), Slot.Size, &Transferred, &Slot.Overlapped))
    {
        if (GetLastError() != ERROR_IO_PENDING)
        {
            return false;
        }
        Slot.Pending = true;
    }
    NextWriteSlot = (NextWriteSlot + 1) % WriteSlots.size();
    return true;
}

bool UsbAccessoryLink::WriteAudio(const std::int16_t* Samples, std::size_t Count)
{
    if (InterfaceHandle == nullptr || Samples == nullptr)
    {
        return false;
    }
    if (Count == 0)
    {
        return true;
    }
    return WriteBytes(reinterpret_cast<const std::uint8_t*>(Samples), Count * sizeof(*Samples));
}

void UsbAccessoryLink::CancelTransfers()
{
    if (InterfaceHandle != nullptr)
    {
        if (OutPipe != 0)
        {
            WinUsb_AbortPipe(InterfaceHandle, OutPipe);
        }
        if (InPipe != 0)
        {
            WinUsb_AbortPipe(InterfaceHandle, InPipe);
        }
    }
}

void UsbAccessoryLink::Close()
{
    if (InterfaceHandle != nullptr)
    {
        CancelTransfers();
        for (WriteSlot& Slot : WriteSlots)
        {
            if (Slot.Pending)
            {
                ULONG Transferred = 0;
                WinUsb_GetOverlappedResult(InterfaceHandle, &Slot.Overlapped, &Transferred, TRUE);
                Slot.Pending = false;
            }
        }
        WinUsb_Free(InterfaceHandle);
        InterfaceHandle = nullptr;
    }
    for (WriteSlot& Slot : WriteSlots)
    {
        Slot.Pending = false;
        if (Slot.Event != nullptr)
        {
            CloseHandle(Slot.Event);
            Slot.Event = nullptr;
        }
    }
    NextWriteSlot = 0;
    if (DeviceHandle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(DeviceHandle);
        DeviceHandle = INVALID_HANDLE_VALUE;
    }
    OutPipe = 0;
    InPipe = 0;
}

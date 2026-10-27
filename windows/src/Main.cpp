#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <avrt.h>

#include "AudioCapture.h"
#include "UsbAccessory.h"

namespace
{
    std::atomic<bool> AppRunning{true};
    std::atomic<bool> SessionRunning{false};
    constexpr std::chrono::milliseconds ReconnectInterval{250};
    constexpr std::chrono::milliseconds RejectedRetryInterval{3000};
    constexpr std::chrono::milliseconds CaptureRetryInterval{500};
    constexpr std::uint32_t CaptureRetryLimit = 10;

    BOOL WINAPI ConsoleControl(DWORD ControlType)
    {
        if (ControlType == CTRL_C_EVENT || ControlType == CTRL_BREAK_EVENT || ControlType == CTRL_CLOSE_EVENT || ControlType == CTRL_SHUTDOWN_EVENT)
        {
            AppRunning.store(false, std::memory_order_release);
            SessionRunning.store(false, std::memory_order_release);
            return TRUE;
        }
        return FALSE;
    }

    void ConfigureProcessScheduling()
    {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        PROCESS_POWER_THROTTLING_STATE Throttling{};
        Throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        Throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        Throttling.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &Throttling, sizeof(Throttling));
    }

    struct AutoConnectState
    {
        std::vector<UsbDeviceId> Present;
        std::vector<UsbDeviceId> Rejected;
        UsbDeviceId Preferred{};
        bool HasPreferred = false;
        std::chrono::steady_clock::time_point RejectedSince = std::chrono::steady_clock::now();
    };

    std::int64_t RemainingMilliseconds(std::chrono::steady_clock::time_point Deadline)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - std::chrono::steady_clock::now()).count();
    }

    bool TryAutoConnect(UsbAccessoryLink& Link, std::atomic<bool>& Running, AutoConnectState& State, std::uint32_t TimeoutMilliseconds)
    {
        std::vector<UsbDeviceId> Present = FindUsbDeviceIds();
        const auto Now = std::chrono::steady_clock::now();
        if (Present != State.Present || Now - State.RejectedSince >= RejectedRetryInterval)
        {
            State.Present = Present;
            State.Rejected.clear();
            State.RejectedSince = Now;
        }
        if (State.HasPreferred)
        {
            const auto PreferredIterator = std::find(Present.begin(), Present.end(), State.Preferred);
            if (PreferredIterator != Present.end())
            {
                std::rotate(Present.begin(), PreferredIterator, PreferredIterator + 1);
            }
        }

        for (const UsbDeviceId& DeviceId : Present)
        {
            if (!Running.load(std::memory_order_acquire))
            {
                return false;
            }
            if (std::find(State.Rejected.begin(), State.Rejected.end(), DeviceId) != State.Rejected.end())
            {
                continue;
            }
            if (!Link.EnterAccessoryMode(DeviceId.VendorId, DeviceId.ProductId))
            {
                State.Rejected.push_back(DeviceId);
                continue;
            }

            State.Preferred = DeviceId;
            State.HasPreferred = true;
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMilliseconds);
            if (!Link.WaitForAccessory(Running, TimeoutMilliseconds))
            {
                return false;
            }
            const std::int64_t Remaining = RemainingMilliseconds(Deadline);
            return Remaining > 0 && Link.WaitForReceiver(Running, static_cast<std::uint32_t>(Remaining));
        }
        return false;
    }
}

int wmain(int ArgumentCount, wchar_t** Arguments)
{
    SetConsoleCtrlHandler(ConsoleControl, TRUE);
    ConfigureProcessScheduling();
    std::uint32_t ConnectTimeoutMilliseconds = 5000;
    for (int ArgumentIndex = 1; ArgumentIndex < ArgumentCount; ++ArgumentIndex)
    {
        const std::wstring Option = Arguments[ArgumentIndex];
        if (Option != L"--timeout" || ArgumentIndex + 1 >= ArgumentCount)
        {
            return 2;
        }
        wchar_t* End = nullptr;
        const unsigned long TimeoutSeconds = std::wcstoul(Arguments[++ArgumentIndex], &End, 10);
        if (End == nullptr || *End != L'\0' || TimeoutSeconds == 0 || TimeoutSeconds > 300)
        {
            return 2;
        }
        ConnectTimeoutMilliseconds = static_cast<std::uint32_t>(TimeoutSeconds * 1000);
    }
    AutoConnectState AutoState;

    while (AppRunning.load(std::memory_order_acquire))
    {
        UsbAccessoryLink Link;
        bool Connected = Link.TryOpenAccessory();
        if (Connected)
        {
            Connected = Link.WaitForReceiver(AppRunning);
        }
        else
        {
            Connected = TryAutoConnect(Link, AppRunning, AutoState, ConnectTimeoutMilliseconds);
        }
        if (!Connected)
        {
            Link.Close();
            if (!AppRunning.load(std::memory_order_acquire))
            {
                break;
            }
            std::this_thread::sleep_for(ReconnectInterval);
            continue;
        }

        std::wcout << L"USB connected.\n";
        SessionRunning.store(true, std::memory_order_release);
        SpscQueue<AudioPacket, AudioQueueCapacity> Queue;
        HANDLE QueueEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (QueueEvent == nullptr)
        {
            SessionRunning.store(false, std::memory_order_release);
            Link.Close();
            return 1;
        }
        std::thread Sender([&]()
        {
            DWORD TaskIndex = 0;
            HANDLE MmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &TaskIndex);
            if (MmcssHandle != nullptr)
            {
                AvSetMmThreadPriority(MmcssHandle, AVRT_PRIORITY_HIGH);
            }
            else
            {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            }
            AudioPacket Packet;
            while (SessionRunning.load(std::memory_order_acquire))
            {
                if (Queue.Pop(Packet))
                {
                    if (!Link.WriteAudio(Packet.Samples.data(), Packet.Count))
                    {
                        SessionRunning.store(false, std::memory_order_release);
                        break;
                    }
                }
                else
                {
                    WaitForSingleObject(QueueEvent, INFINITE);
                }
            }
            if (MmcssHandle != nullptr)
            {
                AvRevertMmThreadCharacteristics(MmcssHandle);
            }
        });
        bool CaptureGaveUp = false;
        std::uint32_t ConsecutiveFailures = 0;
        while (SessionRunning.load(std::memory_order_acquire))
        {
            AudioCapture Capture;
            const CaptureResult Result = Capture.Run(SessionRunning, Queue, QueueEvent);
            if (Result == CaptureResult::Stopped)
            {
                break;
            }
            if (Result == CaptureResult::DeviceChanged)
            {
                ConsecutiveFailures = 0;
                continue;
            }
            if (++ConsecutiveFailures >= CaptureRetryLimit)
            {
                CaptureGaveUp = true;
                break;
            }
            std::this_thread::sleep_for(CaptureRetryInterval);
        }
        SessionRunning.store(false, std::memory_order_release);
        SetEvent(QueueEvent);
        Link.CancelTransfers();
        if (Sender.joinable())
        {
            Sender.join();
        }
        Link.Close();
        CloseHandle(QueueEvent);
        if (CaptureGaveUp)
        {
            return 1;
        }
    }
    return 0;
}
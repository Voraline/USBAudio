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
    constexpr std::chrono::milliseconds CandidateProbeInterval{250};
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

    bool ParseId(const std::wstring& Value, std::uint16_t& Result)
    {
        wchar_t* End = nullptr;
        const unsigned long Parsed = std::wcstoul(Value.c_str(), &End, 16);
        if (Value.empty() || End == nullptr || *End != L'\0' || Parsed > 0xFFFF)
        {
            return false;
        }
        Result = static_cast<std::uint16_t>(Parsed);
        return true;
    }

    bool TryConnectCandidate(UsbAccessoryLink& Link, std::atomic<bool>& Running, std::uint16_t VendorId, std::uint16_t ProductId, std::uint32_t TimeoutMilliseconds)
    {
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMilliseconds);
        while (Running.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < Deadline)
        {
            if (Link.TryOpenAccessory())
            {
                const auto Remaining = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - std::chrono::steady_clock::now()).count();
                return Remaining > 0 && Link.WaitForReceiver(Running, static_cast<std::uint32_t>(Remaining));
            }
            if (Link.EnterAccessoryMode(VendorId, ProductId))
            {
                const auto Remaining = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - std::chrono::steady_clock::now()).count();
                if (Remaining <= 0 || !Link.WaitForAccessory(Running, static_cast<std::uint32_t>(Remaining)))
                {
                    return false;
                }
                const auto ReceiverRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(Deadline - std::chrono::steady_clock::now()).count();
                return ReceiverRemaining > 0 && Link.WaitForReceiver(Running, static_cast<std::uint32_t>(ReceiverRemaining));
            }
            std::this_thread::sleep_for(CandidateProbeInterval);
        }
        return false;
    }
}

int wmain(int ArgumentCount, wchar_t** Arguments)
{
    SetConsoleCtrlHandler(ConsoleControl, TRUE);
    ConfigureProcessScheduling();
    std::vector<std::uint16_t> VendorIds;
    std::vector<std::uint16_t> ProductIds;
    std::uint32_t CandidateTimeoutMilliseconds = 5000;
    for (int ArgumentIndex = 1; ArgumentIndex < ArgumentCount; ++ArgumentIndex)
    {
        const std::wstring Option = Arguments[ArgumentIndex];
        if (Option == L"--timeout" && ArgumentIndex + 1 < ArgumentCount)
        {
            wchar_t* End = nullptr;
            const unsigned long TimeoutSeconds = std::wcstoul(Arguments[++ArgumentIndex], &End, 10);
            if (End == nullptr || *End != L'\0' || TimeoutSeconds == 0 || TimeoutSeconds > 300)
            {
                return 2;
            }
            CandidateTimeoutMilliseconds = static_cast<std::uint32_t>(TimeoutSeconds * 1000);
            continue;
        }
        std::vector<std::uint16_t>* Values = nullptr;
        if (Option == L"--vid")
        {
            Values = &VendorIds;
        }
        else if (Option == L"--pid")
        {
            Values = &ProductIds;
        }
        else
        {
            return 2;
        }

        while (ArgumentIndex + 1 < ArgumentCount)
        {
            const std::wstring Value = Arguments[ArgumentIndex + 1];
            if (Value.rfind(L"--", 0) == 0)
            {
                break;
            }
            std::uint16_t Id = 0;
            if (!ParseId(Value, Id))
            {
                return 2;
            }
            Values->push_back(Id);
            ++ArgumentIndex;
        }
        if (Values->empty())
        {
            return 2;
        }
    }
    if (VendorIds.size() != ProductIds.size())
    {
        return 2;
    }

    while (AppRunning.load(std::memory_order_acquire))
    {
        UsbAccessoryLink Link;
        bool Connected = Link.TryOpenAccessory();
        if (Connected)
        {
            Connected = Link.WaitForReceiver(AppRunning);
        }
        else if (VendorIds.empty())
        {
            return 1;
        }
        for (std::size_t CandidateIndex = 0; !Connected && CandidateIndex < VendorIds.size() && AppRunning.load(std::memory_order_acquire); ++CandidateIndex)
        {
            Connected = TryConnectCandidate(Link, AppRunning, VendorIds[CandidateIndex], ProductIds[CandidateIndex], CandidateTimeoutMilliseconds);
        }
        if (!Connected)
        {
            Link.Close();
            if (!AppRunning.load(std::memory_order_acquire))
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
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
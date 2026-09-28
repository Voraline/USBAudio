#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <thread>

#include <windows.h>

#include "AudioCapture.h"
#include "UsbAccessory.h"

namespace
{
    std::atomic<bool> AppRunning{true};
    std::atomic<bool> SessionRunning{false};

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
}

int wmain(int ArgumentCount, wchar_t** Arguments)
{
    SetConsoleCtrlHandler(ConsoleControl, TRUE);
    std::uint16_t VendorId = 0;
    std::uint16_t ProductId = 0;
    bool HasVendorId = false;
    bool HasProductId = false;
    for (int ArgumentIndex = 1; ArgumentIndex < ArgumentCount; ++ArgumentIndex)
    {
        const std::wstring Argument = Arguments[ArgumentIndex];
        if (Argument == L"--vid" && ArgumentIndex + 1 < ArgumentCount)
        {
            HasVendorId = ParseId(Arguments[++ArgumentIndex], VendorId);
        }
        else if (Argument == L"--pid" && ArgumentIndex + 1 < ArgumentCount)
        {
            HasProductId = ParseId(Arguments[++ArgumentIndex], ProductId);
        }
        else
        {
            std::wcerr << L"Usage: UsbAudioSender.exe [--vid XXXX --pid YYYY]\n";
            return 2;
        }
    }
    if (HasVendorId != HasProductId)
    {
        std::wcerr << L"Supply both --vid and --pid, or neither when the phone is already in accessory mode.\n";
        return 2;
    }

    while (AppRunning.load(std::memory_order_acquire))
    {
        UsbAccessoryLink Link;
        bool Connected = Link.TryOpenAccessory();
        if (!Connected && !HasVendorId)
        {
            std::wcerr << L"Phone is not in accessory mode. Start the app with its current phone --vid and --pid to enable USB negotiation and automatic reconnect.\n";
            return 1;
        }
        if (!Connected && Link.EnterAccessoryMode(VendorId, ProductId))
        {
            Connected = Link.WaitForAccessory(AppRunning);
        }
        if (!Connected)
        {
            if (!AppRunning.load(std::memory_order_acquire))
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        SessionRunning.store(true, std::memory_order_release);
        SpscQueue<AudioPacket, 17> Queue;
        HANDLE QueueEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (QueueEvent == nullptr)
        {
            SessionRunning.store(false, std::memory_order_release);
            Link.Close();
            return 1;
        }
        std::thread Sender([&]()
        {
            AudioPacket Packet;
            while (SessionRunning.load(std::memory_order_acquire))
            {
                if (Queue.Pop(Packet))
                {
                    if (!Link.WriteAudio(Packet.Sequence, Packet.TimestampNs, Packet.Payload.data(), Packet.PayloadSize))
                    {
                        if (SessionRunning.load(std::memory_order_acquire))
                        {
                            std::wcerr << L"USB audio write failed. Waiting for a reconnect.\n";
                        }
                        SessionRunning.store(false, std::memory_order_release);
                        break;
                    }
                }
                else
                {
                    WaitForSingleObject(QueueEvent, 1000);
                }
            }
        });
        std::thread Feedback([&]()
        {
            if (!Link.ReadFeedback(SessionRunning) && SessionRunning.load(std::memory_order_acquire))
            {
                SessionRunning.store(false, std::memory_order_release);
            }
        });

        AudioCapture Capture;
        const bool CaptureStarted = Capture.Run(SessionRunning, Queue, QueueEvent);
        SessionRunning.store(false, std::memory_order_release);
        SetEvent(QueueEvent);
        Link.CancelTransfers();
        if (Sender.joinable())
        {
            Sender.join();
        }
        if (Feedback.joinable())
        {
            Feedback.join();
        }
        Link.Close();
        CloseHandle(QueueEvent);
        if (!CaptureStarted)
        {
            return 1;
        }
        if (AppRunning.load(std::memory_order_acquire))
        {
            std::wcout << L"Waiting for the USB audio connection to return...\n";
        }
    }
    return 0;
}

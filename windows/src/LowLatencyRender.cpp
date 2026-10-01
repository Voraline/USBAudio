#include "LowLatencyRender.h"

LowLatencyRender::~LowLatencyRender()
{
    Close();
}

bool LowLatencyRender::Open(IMMDevice* Device, const WAVEFORMATEX* Format)
{
    Close();
    UINT32 DefaultPeriod = 0;
    UINT32 FundamentalPeriod = 0;
    UINT32 MinimumPeriod = 0;
    UINT32 MaximumPeriod = 0;
    RenderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (RenderEvent == nullptr ||
        FAILED(Device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&Client))) ||
        FAILED(Client->GetSharedModeEnginePeriod(Format, &DefaultPeriod, &FundamentalPeriod, &MinimumPeriod, &MaximumPeriod)) ||
        MinimumPeriod >= DefaultPeriod ||
        FAILED(Client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, MinimumPeriod, Format, nullptr)) ||
        FAILED(Client->SetEventHandle(RenderEvent)) ||
        FAILED(Client->GetBufferSize(&BufferFrames)) ||
        FAILED(Client->GetService(IID_PPV_ARGS(&Render))))
    {
        Close();
        return false;
    }
    FillSilence();
    if (FAILED(Client->Start()))
    {
        Close();
        return false;
    }
    return true;
}

void LowLatencyRender::FillSilence()
{
    if (Client == nullptr || Render == nullptr)
    {
        return;
    }
    UINT32 Padding = 0;
    if (FAILED(Client->GetCurrentPadding(&Padding)) || Padding >= BufferFrames)
    {
        return;
    }
    const UINT32 Available = BufferFrames - Padding;
    BYTE* Data = nullptr;
    if (SUCCEEDED(Render->GetBuffer(Available, &Data)))
    {
        Render->ReleaseBuffer(Available, AUDCLNT_BUFFERFLAGS_SILENT);
    }
}

void LowLatencyRender::Close()
{
    if (Client != nullptr)
    {
        Client->Stop();
    }
    if (Render != nullptr)
    {
        Render->Release();
        Render = nullptr;
    }
    if (Client != nullptr)
    {
        Client->Release();
        Client = nullptr;
    }
    if (RenderEvent != nullptr)
    {
        CloseHandle(RenderEvent);
        RenderEvent = nullptr;
    }
    BufferFrames = 0;
}

HANDLE LowLatencyRender::GetEvent() const
{
    return RenderEvent;
}

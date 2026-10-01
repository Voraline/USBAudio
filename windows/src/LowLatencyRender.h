#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

class LowLatencyRender
{
public:
    LowLatencyRender() = default;
    ~LowLatencyRender();
    LowLatencyRender(const LowLatencyRender&) = delete;
    LowLatencyRender& operator=(const LowLatencyRender&) = delete;

    bool Open(IMMDevice* Device, const WAVEFORMATEX* Format);
    void FillSilence();
    void Close();
    HANDLE GetEvent() const;

private:
    IAudioClient3* Client = nullptr;
    IAudioRenderClient* Render = nullptr;
    HANDLE RenderEvent = nullptr;
    UINT32 BufferFrames = 0;
};

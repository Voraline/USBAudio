# USB Audio

USB Audio streams Windows system playback audio to an Android phone in real time. The Windows app captures the default render endpoint with WASAPI loopback, converts it to 48 kHz stereo, encodes 20 ms frames with Opus, and sends framed packets over Android Open Accessory bulk USB endpoints. The Android app validates packet checksums, decodes Opus in native C++, maintains a bounded PCM ring buffer, corrects clock drift from queue-depth feedback, and plays through AAudio.

All audio and feedback data travels over the USB accessory bulk endpoints. The project does not use Wi-Fi, Bluetooth, TCP/IP, sockets, or tethering.

## Project layout

- `windows/` contains the Windows WASAPI capture, Opus encoding, AOA negotiation, and WinUSB transport.
- `android/` contains the accessory-mode Android app, JNI bridge, Opus decoder, jitter buffer, clock correction, and AAudio playback.
- `shared/Protocol.h` defines the USB packet format and CRC.
- `.github/workflows/build.yml` builds the Windows executable and Android release APK on pushes and pull requests.

## Requirements

The Android phone must support USB accessory mode and run Android 9 (API 28) or later. Use a USB data cable and approve the USB accessory prompt for USB Audio Receiver. Android playback runs in a foreground media service so streaming can continue when the activity is backgrounded.

Windows needs Visual Studio 2022 C++ build tools, CMake 3.24 or later, and Git. Android builds need JDK 17, Gradle 8.9, Android SDK Platform 35, Android Build Tools 35.0.0, NDK 27.2.12479018, and CMake 3.22.1. Gradle and CMake fetch Opus v1.5.2 automatically.

The Windows host also needs a WinUSB driver bound to the phone's current USB interface and the Android accessory interface. Windows does not expose arbitrary vendor USB control transfers to a desktop app without a suitable function driver. Use [Zadig](https://zadig.akeo.ie/) to bind WinUSB to the phone interface while the phone is in normal USB mode, then bind WinUSB to the Android Open Accessory interface if Windows does not retain a usable WinUSB binding after the phone switches modes. Select the individual Android USB interface rather than replacing drivers for unrelated phone functions. The sender discovers the interface GUIDs registered by the installed driver, so no project INF or custom interface GUID is needed. This driver setup is required for hardware use and does not change the application build.

## Build the Windows app

Run these commands in a Visual Studio 2022 x64 Native Tools PowerShell prompt from the repository root:

```powershell
cmake -S . -B build/windows -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release --parallel
```

The executable is `build/windows/Release/UsbAudioSender.exe`.

Find the phone's current vendor ID and product ID in Device Manager under the connected phone interface's **Details > Hardware Ids**. With the phone in normal USB mode, run:

```powershell
.\build\windows\Release\UsbAudioSender.exe --vid 1234 --pid 5678
```

If the phone is already in accessory mode, the sender can start without VID/PID arguments. Supplying the normal-mode IDs allows the sender to request accessory mode and retry the connection after a disconnect.

## Build the Android release APK

From the repository root, after installing the Android build requirements:

```powershell
cd android
gradle --no-daemon assembleRelease
```

The APK is `android/app/build/outputs/apk/release/app-release.apk`. The release build uses the Android debug signing configuration and does not need a custom signing key.

## GitHub Actions artifacts

Push the project to GitHub. The workflow builds both targets on each push and pull request and uploads:

- `windows-usb-audio`, containing `UsbAudioSender.exe`
- `android-release-apk`, containing `app-release.apk`

## Latency and recovery

Audio is framed in 20 ms Opus packets. The receiver starts after 40 ms of queued PCM and targets a 50 ms buffer. AAudio uses low-latency mode and a small device buffer. Android reports queue depth every 250 ms; the receiver adjusts its consumption rate by at most 0.5% to track the independent device clocks. Sequence gaps and CRC failures are handled with Opus packet-loss concealment. A disconnect stops the active stream cleanly; the Windows sender retries the USB accessory handshake when phone VID/PID arguments were provided.

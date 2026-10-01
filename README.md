# USB Audio

USB Audio streams Windows system playback audio to an Android phone over USB. The Windows app captures the default render endpoint with WASAPI loopback, downmixes it to 48 kHz mono signed 16-bit PCM, and sends fixed 1.25 ms audio blocks over Android Open Accessory bulk USB. The Android app copies those blocks into a bounded PCM ring buffer and plays them through AAudio.

Audio travels over the USB accessory bulk endpoint. The project does not use Wi-Fi, Bluetooth, TCP/IP, sockets, or tethering.

## Project layout

- `windows/` contains the Windows WASAPI capture, PCM conversion, AOA negotiation, and WinUSB transport.
- `android/` contains the accessory-mode Android app, JNI bridge, bounded PCM ring buffer, and AAudio playback.
- `shared/AudioFormat.h` defines the PCM format and receiver-ready signal.
- `.github/workflows/build.yml` builds the Windows executable and Android release APK on pushes and pull requests.

## Requirements

The Android phone must support USB accessory mode, the `arm64-v8a` ABI, and Android 13 (API 33) or later. Use a USB data cable and approve the USB accessory prompt for USB Audio Receiver. Android playback runs in a foreground media service so streaming can continue when the activity is backgrounded.

Windows needs an AVX2-capable x64 CPU, Visual Studio 2022 C++ build tools, and CMake 3.24 or later. The executable is compiled with AVX2 and will not run on CPUs without AVX2 support. Android builds need JDK 17, Gradle 8.9, Android SDK Platform 35, Android Build Tools 35.0.0, NDK 27.2.12479018, and CMake 3.22.1.

The Windows sender uses a WinUSB driver already bound by Windows to the phone's normal USB interface and Android accessory interface. It discovers the registered interface GUIDs automatically and does not install a driver. Windows can bind its built-in WinUSB driver without a separate package only when the USB device firmware advertises the required Microsoft OS descriptors. Android Open Accessory does not guarantee those descriptors, so phones without an existing compatible driver binding cannot be used by this Windows application through AOA alone. Installing a signed driver package or changing the phone firmware would be required to support those devices. AOA's optional standard USB audio mode sends audio from the Android device to its accessory, so it cannot replace the bulk-data path used here.

## Build the Windows app

Run these commands in a Visual Studio 2022 x64 Native Tools PowerShell prompt from the repository root:

```powershell
cmake -S . -B build/windows -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --config Release --parallel
```

The executable is `build/windows/Release/UsbAudioSender.exe`.

The sender accepts multiple vendor and product IDs. Pair values by position; it waits up to 5 seconds for each pair by default before trying the next. Set `--timeout N` to use a different per-pair timeout in seconds. Find the phone's normal-mode IDs in Device Manager under the connected phone interface's **Details > Hardware Ids**. For example:

```powershell
.\build\windows\Release\UsbAudioSender.exe --vid 18D1 2717 --pid 4EE7 FF88 --timeout 2
```

The sender tries `18D1:4EE7` first, then `2717:FF88`. The example gives each pair 2 seconds. Timeout values must be between 1 and 300 seconds. If the phone is already in accessory mode, the sender detects it without VID/PID arguments. Supplying normal-mode IDs allows the sender to request accessory mode and retry after disconnects. The only console output is printed when the USB audio link is ready.

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

Audio is sent as 60-sample, 1.25 ms blocks of mono signed 16-bit PCM. This is 96,000 bytes per second at 48 kHz. The Windows sender waits for a one-byte receiver-ready signal before streaming so both sides begin on a block boundary. Android repeats the ready signal every 500 ms while active, allowing the Windows sender to restart without unplugging the phone. AAudio uses low-latency mode and requests at least one device burst, with a 192-frame minimum buffer. Clock-drift correction is disabled. USB has ample throughput for this PCM rate, but the laptop and phone audio clocks remain independent. The Android ring is bounded to 1,024 samples (about 21.3 ms); when the clocks diverge, a full ring drops incoming blocks and an empty ring produces silence rather than allowing latency to grow without bound. A disconnect stops the active stream cleanly; the Windows sender retries the USB accessory handshake when phone VID/PID arguments were provided.

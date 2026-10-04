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

Audio is mono signed 16-bit PCM at 48 kHz, which is 96,000 bytes per second. The Windows sender does not wait to fill a fixed block. Each WASAPI capture event is converted and flushed immediately as one or more bulk transfers of at most 480 samples, so no captured sample waits for a packet to fill. Transfers that are a multiple of the endpoint packet size are followed by a zero-length packet so the Android accessory driver completes the read at once, and up to four writes are kept in flight on the bulk OUT pipe. Samples are converted to 16-bit with TPDF dither; digital silence passes through exactly.

The Windows sender waits for a one-byte receiver-ready signal before streaming. Android repeats the ready signal every 500 ms while active, allowing the Windows sender to restart without unplugging the phone. If the default playback device changes or is removed, capture restarts on the new device without dropping the USB link.

The Android receiver reads whatever bytes arrive, carrying an odd trailing byte across reads, into a 2,048-sample ring. The laptop and phone audio clocks are independent, so playback reads the ring through a 16-tap windowed-sinc interpolator whose step is adjusted by a PI controller from the smoothed ring fill. This holds the buffer at a constant target with rate changes limited to 0.2 percent, instead of letting it drift until samples are dropped or repeated. The interpolator's lookahead comes from data already buffered, so it adds no latency.

The target fill is the larger of 240 frames and the AAudio callback size plus 128 frames. If the ring starves, output decays smoothly, the target grows by 32 frames up to 512 extra, and playback re-primes before resuming with a short fade-in. The buffer therefore settles at the smallest size the phone and USB path can sustain, and the extra target resets whenever the stream restarts. If the ring ever holds more than 960 frames above target, playback skips to the target with a 48-frame crossfade. Fill, target, controller adjustment in ppm, underruns, resyncs, and dropped chunks are written to logcat under the tag `UsbAudio` every 10 seconds and whenever an event counter changes.

AAudio uses low-latency mode and requests at least one device burst, with a 192-frame minimum buffer. USB has ample throughput for this PCM rate. A disconnect stops the active stream cleanly; the Windows sender retries the USB accessory handshake when phone VID/PID arguments were provided.

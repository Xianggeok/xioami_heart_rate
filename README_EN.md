# Xiaomi Mi Band Heart Rate (OBS Live)

A desktop application that reads heart rate from Xiaomi Mi Band 10's "Heart Rate Broadcast" feature. Chinese interface, supports **real-time heart rate display for OBS live streaming**, system tray, and single-file execution. Interface based on [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) framework.

## Features

- Reads heart rate via Bluetooth Low Energy (BLE): Scans for devices broadcasting Heart Rate Service (`0x180D`), connects and subscribes to Heart Rate Measurement characteristic (`0x2A37`).
- **OBS Browser Source**: Built-in local HTTP server provides real-time heart rate as a webpage for OBS. Add a "Browser" source in OBS with address `http://127.0.0.1:3030` (`/` for overlay page, `/api` returns JSON).
- Chinese dashboard: Large font heart rate display, wearing status, connection status, min/avg/max values, heart rate history line chart with specific BPM values.
- System tray: Close or minimize window to hide to tray, continue running in background; tray menu "Show" to restore, "Exit" to quit.
- **Single exe release**: Icons, fonts, and other resources embedded in exe, automatically extracted to temp directory on startup, no need to carry `assets` folder.
- Window freely resizable, small windows automatically reduce font size and margins.

## Requirements

- Windows 10 (2004 or later) / Windows 11.
- Bluetooth Low Energy adapter (most laptops/desktops have built-in).
- Compilation: Visual Studio 2022+ (with "Desktop development with C++"), CMake 3.14+, Windows SDK ≥ 10.0.22000 (BLE uses SDK's built-in C++/WinRT headers).

## Compilation

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DEUI_BUILD_APPS=OFF -DEUI_BUILD_USER_APPS=ON -DEUI_BUILD_TEST_FIXTURES=OFF
cmake --build build --target heart_rate --config Release
```

Executable output is at `build\heart_rate.exe` (single file, no `assets` directory needed).

> If your Windows SDK is older (< 10.0.22000), you need to specify additional modern C++/WinRT header directory
> (can be generated with `Microsoft.Windows.CppWinRT` tool, directory must contain `winrt/*.h`):
> `-DHEART_RATE_CPPWINRT_DIR=D:\path\to\cppwinrt\headers`.

## Usage

1. Enable heart rate broadcast on the band: **Settings → Heart Rate Broadcast → Enable**.
2. Run `heart_rate.exe`.
3. Click "Start", the program automatically scans, connects, and displays heart rate.
4. Add browser source in OBS: `http://127.0.0.1:3030`.
5. Close or minimize window → hide to tray.

## Directory Structure

```
apps/heart_rate/
  app.cpp              # Chinese dashboard + custom line chart + tray + embedded resource extraction
  assets_embedded.h    # Embedded icon/font byte arrays (single exe packaging, auto-generated)
  ble_heart_rate.h/.cpp# C++/WinRT scanning / connection / notification
  heart_rate_http.h/.cpp # OBS browser source HTTP server (/ overlay page + /api JSON)

core/ include/ components/ modules/  # EUI-NEO framework
3rd/                                # Dependencies (GLFW / FreeType / zlib / libpng ...)
assets/                             # Icon and font source files (source for generating embedded header files)
```

## License and Acknowledgments

- UI Framework: [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) (Apache-2.0, see `LICENSE`).
- Heart rate reading logic ported from [miband-heart-rate](https://github.com/Tnze/miband-heart-rate)
  (MIT, © 2023 Tnze), see `THIRD_PARTY_NOTICES.md`.

## Credits

- [miband-heart-rate](https://github.com/Tnze/miband-heart-rate) - BLE heart rate reading logic
- [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) - UI framework
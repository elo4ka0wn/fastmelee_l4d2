# Fast Melee Macro

Cross-platform Qt application that automates a "2 → wait → left click → 1/4/5" input cycle while a configurable hotkey is held. The macro stops immediately when the hotkey is released and lets you configure per-action delays and target key values.

## Features

- GUI built with Qt to configure the trigger key, per-action delays, and the final key (1, 4, or 5).
- Global hotkey monitoring with low-latency polling on Linux (via evdev devices compatible with Wayland compositors) and a low-level keyboard hook on Windows.
- Input simulation implemented with platform APIs (`SendInput` on Windows, a `/dev/uinput` virtual device on Linux) that avoids blocking user input.
- Profile persistence using a JSON configuration stored in the user's application configuration directory.
- Embedded application icon.

## Building

### Dependencies

- **Qt 6.2+ Widgets module**
- A C++17 compiler
- **Windows:** no additional dependencies beyond the Windows SDK.
- **Linux (Wayland):** kernel `uinput` support and permission to read from `/dev/input/event*` and write to `/dev/uinput` (for example by running inside the `input` group or via custom udev rules).

### Steps

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Windows you can run the above from a Visual Studio Developer Prompt. On Linux ensure the required development packages are installed.

> **Note:** The Linux build expects the `uinput` kernel module to be loaded. You can check with `lsmod | grep uinput` or enable it with `sudo modprobe uinput`.

The resulting executable is `build/FastMeleeMacro` (or `FastMeleeMacro.exe` on Windows).

## Usage

1. Launch the application.
2. Click **Bind key** and press the key you want to use as the macro trigger. Only single keys without modifiers are supported.
3. Adjust the delays (in milliseconds) before pressing `2`, the left mouse button, and the final key. Default values are 0 ms, 300 ms, and 0 ms respectively.
4. Choose whether the final key should be `1`, `4`, or `5`.
5. (Optional) Click **Save profile** to persist your configuration.

Hold the bound key to start the macro. Release it to stop immediately. The macro runs entirely in the background, allowing other keys and mouse interactions to continue functioning normally.

# IME Steady

**Keep your taskbar steady. No more shifting IME indicators.**

A lightweight Windows tray indicator that keeps the taskbar steady while switching between English (US) and Microsoft Pinyin.

## Why?

Windows' built-in language and input-mode indicators take up different amounts of space. Switch between English and Chinese, and the surrounding taskbar icons shift back and forth.

**IME Steady replaces the shifting indicators with a fixed-width tray icon.** Your input mode still changes; the rest of your taskbar stays put.

### Before: Windows' built-in indicators

https://github.com/user-attachments/assets/59a63f25-ac9a-42fe-b7f9-86e84e01d84c

### After: IME Steady

https://github.com/user-attachments/assets/fa079c74-32d3-4d15-8b06-6cc3065bcdf9

## Input modes

| Indicator | Meaning |
| :---: | --- |
| `EN` | English (US) keyboard |
| `中` | Microsoft Pinyin, Chinese mode |
| `英` | Microsoft Pinyin, English mode |
| `?` | Unsupported language or mode not yet observed |

IME Steady follows the active input mode automatically, without polling or reading what you type.

## Usage

Requires **Windows 11 x64** with Microsoft Pinyin and an English (US) keyboard.

1. Activate Microsoft Pinyin at least once.
2. Double-click **IMESteady.exe** and approve the UAC prompt.
3. The launcher extracts its runtime files, runs `cheapInject.exe` with
   administrator rights, then starts `IMESteadyTray.exe` after the hook is ready.

The tray runs in the launcher's original user context. Right-click its icon and
choose **Exit** to close it. If the icon is hidden, move it out of the tray's
overflow menu. Runtime files remain in a per-user application-data folder.

## Windows input indicator

Double-click [WindowsIMEIndicator.cmd](WindowsIMEIndicator.cmd) to open a menu
for Windows' own input indicator. Choose **Hide**, **Show**, **Taskbar**,
**Status**, or **Exit**; no PowerShell commands need to be typed.

The menu runs [scripts/WindowsIMEIndicator.ps1](scripts/WindowsIMEIndicator.ps1).
Both files are bundled in `IMESteady.exe` and extracted with the same folder
layout. Direct commands remain available for automation, for example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\WindowsIMEIndicator.ps1 Status
```

`Hide` hides the legacy language bar, `Show` displays it, and `Taskbar` restores
the standard Windows taskbar input indicator. `Status` only reads the current
settings. The existing per-application switching setting is preserved.

Open the menu normally. If Windows denies stopping `ctfmon.exe`, the helper
requests UAC approval for that step only. Your settings are applied to the
original user, and text input restarts from that user's context. If approval is
cancelled or the restart still fails, the settings remain saved; sign out and
back in to apply them.

Exit IME Steady before changing these settings: the script restarts `ctfmon.exe`.
Then activate Microsoft Pinyin and launch IME Steady again to reinstall its hook.

## Build

Install Visual Studio 2022 with **Desktop development with C++** and a Windows
SDK, then run:

```bat
build.cmd
```

The script locates the x64 MSVC environment automatically; no global PATH changes
are needed. You can also use an x64 Developer Command Prompt or select
`vcvars64.bat` through the `VCVARS64` environment variable.

The single executable is written to **`dist/IMESteady.exe`**. MinHook
and the C/C++ runtime are linked statically. No package downloads, Python, or
CMake are required.

## How it works

The foreground keyboard layout identifies the selected language, but cannot by
itself distinguish Chinese and English modes within Microsoft Pinyin.

The observer DLL hooks a conversion-mode update routine in `InputService.dll`
inside `ctfmon.exe`. It locates the routine using the byte pattern
`FF 81 80 00 00 00 8B C2`, calls the original routine, then reads bit 0 of the
conversion value to distinguish Chinese from English.

The observer publishes that mode through shared memory and signals a Windows
event. The tray combines it with the foreground thread's keyboard layout.
A separate window-message hook reports layout changes. Updates are event driven;
the indicator does not collect typed text or composition strings.

## Troubleshooting

The startup dialog includes the extraction folder. Check `injector-*.log` there
when injection fails. Keep the complete dialog message and log when reporting an
issue; `Windows error N` or `GLE = N` is a Windows system error, separate from the
application codes below.

### Observer hook status

If the dialog reports `hookStatus`, use this table:

| Code | Meaning | What to do |
| --- | --- | --- |
| `-1` | `InputService.dll` is not loaded in `ctfmon.exe`. | Activate Microsoft Pinyin, then retry. |
| `-2` | Could not read the module information. | Restart Windows and retry; report the error if it persists. |
| `-3` | The conversion-mode byte signature was not found. | The Windows build may need an updated signature. Restart Windows first if an older observer is still loaded. |
| `-4` | MinHook initialization failed. | Restart Windows and retry; report the error if it persists. |
| `-5` | MinHook could not create the hook. | Restart Windows to unload older hooks; if it persists, report a possible compatibility issue. |
| `-6` | MinHook could not enable the hook. | Restart Windows and retry; check whether security software blocked process modification. |

Status `0` means initialization is pending; `1` means the hook is installed.
Neither is an error. A timeout waiting for the observer means no successful hook
installation was reported within 10 seconds; inspect the injector log and retry
after activating Microsoft Pinyin.

### Tray startup exit codes

If the dialog says `IMESteadyTray.exe exited during startup with code N`:

| Code | Meaning | What to do |
| --- | --- | --- |
| `1` | Could not create or open the shared memory/events. | Restart the application; report persistent permission or resource failures. |
| `2` | Could not register the tray window class. | Restart the application; report the error if it persists. |
| `3` | Could not create the tray's hidden window. | Restart the application; report the error if it persists. |
| `4` | Could not load or install the layout hook. | Keep `langhook64.dll` beside `IMESteadyTray.exe`; close the tray and relaunch to restore missing files. |

If the injector reports that `ctfmon.exe` is missing, activate Microsoft Pinyin
and retry. If UAC approval is cancelled, launch again and approve it to continue.

## Credits and license

| Project | Contribution to IME Steady |
| --- | --- |
| [MinHook](https://github.com/TsudaKageyu/minhook), by Tsuda Kageyu and contributors | Compiled inline-hooking library used to intercept Pinyin's conversion-mode routine. Includes Hacker Disassembler Engine by Vyacheslav Patkov for instruction decoding. BSD-style licenses. |
| [mymspinyin](https://github.com/pkmnya/mymspinyin), by pkm | Source of the `cheapInject` DLL injector and the original project's Pinyin-hooking foundation. MIT license. |
| [Icecovery/IMEIndicator](https://github.com/Icecovery/IMEIndicator) | Design inspiration for a steady, text-based tray indicator. Its code, fonts, and icon assets are not bundled. |

See [LICENSE.txt](LICENSE.txt) and
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) for the full notices. The notices
are also included in the executable and extracted alongside the runtime files.

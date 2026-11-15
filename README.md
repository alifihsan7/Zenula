<p align="center">
  <img src="assets/logos/zenula.svg" width="96" alt="Zenula">
</p>

# Zenula

A dynamic island for Windows. Zenula sits at the top center of your main display as a small dot (or a little owl), grows into a pill while music or a timer runs, and opens a panel when you hover it. See it in action on the [website](https://alifihsan7.github.io/Zenula/).

It is one native process (C++20, Win32, Direct2D, C++/WinRT). Nothing animates or polls while the island is idle, so it stays around 15 MB of memory and near zero CPU.

## Download

Get `Zenula-<version>-win-x64.zip` from [Releases](https://github.com/alifihsan7/Zenula/releases), extract it, then right-click `install.ps1` → **Run with PowerShell**. Zenula installs for your Windows user and adds Start menu and Startup shortcuts. To remove it, exit Zenula from the tray and run `uninstall.ps1`.

Zenula is not code-signed yet, so Windows may show **"Windows protected your PC"** the first time. Choose **More info → Run anyway**.

## What it does

| Panel | |
| --- | --- |
| **Home** | A big clock, the next prayer, and a card for each thing that is running: music, a timer, open tasks, Drop Zone items. |
| **Now Playing** | Cover, title, a spectrum visualizer that doubles as the seek bar, playback controls, and a source picker. Synced lyrics from [LRCLIB](https://lrclib.net) in a side panel or in the pill, with romaji under Japanese lines (optional). |
| **Timers** | A Quick Timer and Focus sessions (Pomodoro, Deep work, Long focus, or your own) on a session timeline, with Do Not Disturb during Focus and a recap of today's Focus. |
| **Scratchpad** | Notes and a task list, saved on your PC. |
| **Drop Zone** | A shelf for files, text, and screenshots; drag them back out into any app. |
| **System** | Brightness, audio output, per-app volume, CPU, RAM, GPU, disks, battery, network, and Bluetooth. |
| **Prayer** | Official SIHAT Kemenag RI times for 517 regencies and cities, on a day arc, with reminders on time or a few minutes early. |

Also:

- Short notices (copied text, charger, low battery, connection, Caps Lock, Bluetooth) split off the pill as a bubble.
- When music and a timer run together, one is the pill and the other a small capsule beside it.
- Camera, microphone, and screen sharing show as icons.
- The accent follows the album artwork, Zenula blue, a color you pick, or a color per panel.
- **Dot only** keeps the island small; hovering still opens the panel. Zenula can hide in fullscreen apps and respects Do Not Disturb.

Open Settings from the tray, from Home, or with `Zenula.exe --settings`.

## Optional extras

- **Browser tabs:** Chrome, Edge, and Brave share one media session. To control each tab, run `enable-browser-tabs.ps1` from the package, then load the `browser` folder as an unpacked extension.
- **Windows app notifications:** the MSIX build can preview incoming notifications. Build it with `scripts\package-msix.ps1` and turn on **Connect Windows apps** in Settings.
- **Romaji for Japanese lyrics:** turn it on in Settings → Features → Now Playing. Zenula uses the Windows Japanese keyboard if it is installed; otherwise it downloads a small reading dictionary once from [this release](https://github.com/alifihsan7/Zenula/releases/tag/romaji-ja-v1).

## Your data

Notes, tasks, today's Focus, and add-ons live in `%LOCALAPPDATA%\Zenith Dot\`; settings live under `HKCU\Software\Zenith Dot`. Zenula goes online only for lyrics (LRCLIB, with the song's title and artist), prayer times (Kemenag), and the romaji dictionary when you turn it on.

## Build from source

Install Visual Studio 2022 (or Build Tools) with **Desktop development with C++**, the Windows SDK, and CMake, then from the repository folder:

```powershell
cmake -S . -B out -G "Visual Studio 17 2022" -A x64
cmake --build out --config Release --parallel
cmake --install out --config Release --prefix dist
```

`dist` then holds the same files as a release zip.

| Path | What it is |
| --- | --- |
| `src/island.*` | The dot, pill, panels, bubbles, animations, and hit testing |
| `src/main.cpp` | Window, tray, input, and the Windows services behind each panel |
| `src/settings_window.cpp` | The Settings window |
| `src/media.*`, `src/lyrics.*`, `src/audio_spectrum.*` | Media sessions, lyrics and romaji, and the visualizer |
| `src/scratchpad.*`, `src/tasks.*`, `src/drop_zone.*` | Notes, tasks, and Drop Zone |
| `src/system.*`, `src/power.*`, `src/desktop.*`, `src/bluetooth.*`, `src/brightness.*` | System status and devices |
| `src/prayer*` | Prayer times and locations |
| `browser/`, `scripts/` | Browser extension, install, and packaging scripts |
| `tools/build_romaji_dict.py` | Builds the romaji dictionary from JMdict |

## Credits

- Lyrics from [LRCLIB](https://lrclib.net).
- Prayer times from SIHAT, Kementerian Agama Republik Indonesia.
- The romaji dictionary is built from [JMdict](https://www.edrdg.org/wiki/index.php/JMdict-EDICT_Dictionary_Project) by the Electronic Dictionary Research and Development Group, used under [CC BY-SA 4.0](https://www.edrdg.org/edrdg/licence.html).

<p align="center">
  <img src="images/NXtify_icon.png" alt="NXtify" width="160">
</p>

<h1 align="center">NXtify</h1>

<p align="center">
  A controller-first streaming music client and background playback overlay for Nintendo Switch homebrew.
</p>

<p align="center">
  <a href="https://github.com/noesio582-boop/NXtify/releases/latest"><img src="https://img.shields.io/github/v/release/noesio582-boop/NXtify?label=release" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/app%20license-GPL--3.0--or--later-1DB954" alt="App license: GPL-3.0-or-later"></a>
  <a href="LICENSES/GPL-2.0.txt"><img src="https://img.shields.io/badge/overlay%20license-GPL--2.0--only-1DB954" alt="Overlay license: GPL-2.0-only"></a>
</p>

> **Disclaimer:** **NXtify** is an independent, unofficial open-source homebrew project for Nintendo Switch. It is **not** affiliated with, endorsed by, sponsored by, or supported by Spotify AB, Nintendo Co., Ltd., or any of their subsidiaries or affiliates. All third-party trademarks, service marks, trade names, and logos are the property of their respective owners.

---

## Overview

**NXtify** brings cloud music streaming and background playback to the Nintendo Switch with a hardware-accelerated 60 fps interface. It consists of two complementary components:

| Component | File | Purpose |
| --- | --- | --- |
| **NXtify App** | `NXtify.nro` | Full-screen homebrew application featuring dynamic OKLab cover palettes, real-time audio spectrum visualizer, synced lyrics (LRCLIB), Canvas mode, sleep timer, bilingual UI (English / Español), and full library/playlist browsing. |
| **NXtify Overlay** | `NXtify.ovl` | Lightweight Ultrahand / Tesla overlay that supervises and controls the background audio service while playing games or browsing the HOME menu. |

---

## Screenshots

<p align="center">
  <b>Library & Playlists</b><br>
  <img src="images/screenshot_playlist.jpg" alt="Library and Playlists View" width="850">
</p>

<p align="center">
  <b>Now Playing & Audio Spectrum Visualizer</b><br>
  <img src="images/screenshot_now_playing.jpg" alt="Now Playing with Spectrum Visualizer" width="850">
</p>

<p align="center">
  <b>Synchronized Lyrics (Canvas Mode)</b><br>
  <img src="images/screenshot_lyrics.jpg" alt="Full-Screen Synced Lyrics" width="850">
</p>

---

## Features

### Full-Screen Application (`NXtify.nro`)
- **Controller-First & Touch UI**: Smooth 60 fps SDL2 interface with dynamic OKLab color palettes extracted from the current album cover, 12-hour clock, Wi-Fi signal indicator, and battery status.
- **Full Library & Playlists**: Browse your Home recommendations, Liked Songs, user playlists (with automatic background count and track preloading), saved Albums, and Artists.
- **Real-Time Visualizer & Canvas**: 44-bar spectrum visualizer on Now Playing and full-screen Canvas mode with cover pulse and synced lyrics.
- **Synced Lyrics**: Automatic online lyrics lookup and line-by-line synchronization via LRCLIB, with local caching on the SD card (`sdmc:/config/NXtify/lyrics-cache/`).
- **Sleep Timer**: Built-in countdown timer (`15 min`, `30 min`, `45 min`, `60 min`, `90 min`, or `End of track`) that automatically pauses playback when time expires.
- **Bilingual Interface**: Instant switching between **English** and **Español** under `Settings › Appearance › Language`.
- **Zero-Key Account Pairing**: Built-in local network discovery (Zeroconf / mDNS) on port `7864`—no developer Client ID or secret configuration required.

### Background Service & Overlay (`NXtify.ovl`)
- **In-Game Playback Control**: Play, pause, skip tracks, seek, toggle shuffle/repeat, and adjust volume directly from the Ultrahand overlay without leaving your game.
- **Overlay Browser**: Browse Home, Search, Library, and current Queue directly from the overlay.
- **Game Event Actions**: Configure automatic playback behavior when opening a game, returning to the HOME menu, or closing a game.

---

## Requirements

- A Nintendo Switch running **Atmosphère** custom firmware and **Homebrew Menu**.
- An active **Premium** streaming account (required by the underlying `cspot` native audio protocol).
- Wi-Fi connection (the Switch and your phone/PC must be on the same local network for initial pairing).
- **Ultrahand Overlay** (or compatible Tesla overlay loader) if you wish to use `NXtify.ovl`.

> **Tip:** Launch `NXtify.nro` via **Title Override** (hold `R` while launching any Switch game until Homebrew Menu opens) so the application has access to full system memory.

---

## Installation & Setup Instructions

### Quick Install (Recommended: All-in-One SD Package)

Download **`NXtify-v0.9.0-SD-Package.zip`** from the [**Releases**](https://github.com/noesio582-boop/NXtify/releases) page and extract its contents directly to the root of your Switch SD card (`sdmc:/`).

It places everything in the exact paths required:
```text
sdmc:/
├── atmosphere/
│   └── contents/
│       └── 4200000000535754/
│           ├── exefs.nsp
│           └── toolbox.json
└── switch/
    ├── NXtify/
    │   └── NXtify.nro
    └── .overlays/
        └── NXtify.ovl
```

Reboot your console once after extracting so Atmosphère loads the background audio sysmodule.

### Manual / Individual Install

If you prefer placing components individually:
- Copy `NXtify.nro` to `sdmc:/switch/NXtify/NXtify.nro`.
- Copy `NXtify.ovl` to `sdmc:/switch/.overlays/NXtify.ovl`.
- If you want in-game background playback, place `exefs.nsp` and `toolbox.json` under `sdmc:/atmosphere/contents/4200000000535754/`.

### 2. Linking Your Account (First-Time Setup)

NXtify does **not** require you to create or enter any API keys.

1. Connect your Nintendo Switch and your phone (or desktop app) to the **same Wi-Fi network**.
2. Launch **NXtify** (`NXtify.nro`) on your Nintendo Switch.
3. If no account is linked, NXtify will display the **"Connect your account" / "Conecta tu cuenta"** screen and start listening on your local network.
4. Open the streaming app on your phone or PC, tap the **Devices / Connect** menu, and select **"Nintendo Switch"**.
5. NXtify will automatically authenticate, save your reusable session token to `sdmc:/config/NXtify/credentials.json`, and begin syncing your library and playlists.

### 3. Changing Language & Logging Out

- **Language**: Go to `Settings › Appearance › Language` (`Configuracion › Apariencia › Idioma`) to switch between **English** and **Español**.
- **Log Out**: Go to `Settings › Library › Linked Account` (`Configuracion › Biblioteca › Cuenta vinculada`) and select **Log out** (`Cerrar sesion`). This deletes your local session and returns immediately to the pairing screen.

---

## Security & Privacy Notice

- Your session credentials are stored locally on your SD card at `sdmc:/config/NXtify/credentials.json`.
- **Never** share `credentials.json` or `playback-session.json` publicly or attach them to bug reports. Both files are excluded by `.gitignore`.

---

## Building from Source

Requires **devkitPro** (`devkitA64`, `libnx`, and `switch-portlibs` including `switch-sdl2`, `switch-sdl2_ttf`, `switch-sdl2_image`, `switch-freetype`, `switch-libjpeg-turbo`, `switch-curl`, `switch-mbedtls`, `switch-ffmpeg`, `switch-mpg123`, `switch-flac`, `switch-libvorbisidec`, `switch-opusfile`).

```bash
# 1. Build the standalone application (NXtify.nro)
cd app
make -j8

# 2. Build the Ultrahand overlay (nxtify-overlay.ovl)
cd ../phase2/overlay
make -j8
```

See [BUILDING.md](BUILDING.md) for full build details.

---

## Credits & Acknowledgments

NXtify stands on the shoulders of several open-source projects and their authors:

- **[@noesio582-boop](https://github.com/noesio582-boop)** — NXtify lead developer, modern Switch port, Pathfinder GraphQL library/playlist synchronization, Zeroconf login flow, bilingual localization, visualizer/lyrics integration, and background service integration.
- **HiroshiYamauchi** — Original creator of the Switch streaming client and background sysmodule architecture (`GPL-3.0-or-later`).
- **[AndryTheBeast](https://github.com/AndryTheBeast)** — Creator of [**Songate**](https://github.com/AndryTheBeast/Songate) for Nintendo Switch, whose hardware-accelerated SDL2 homebrew UI engine, layout system, and audio features form the foundation of the NXtify interface (`GPL-3.0`).
- **[DEVIROYrus](https://github.com/DEVIROYrus)** — Creator of [**Gatefold**](https://github.com/DEVIROYrus/Gatefold), whose original visual design, typography, iconography, and OKLab cover palette extraction algorithm inspire the interface (`MIT License`).
- **[feelfreelinux](https://github.com/feelfreelinux)** & contributors — Creators of [**cspot**](https://github.com/feelfreelinux/cspot) (`bell` & `cspot`), providing the C++ native streaming and authentication protocol implementation (`GPL-3.0-or-later`).
- **[ppkantorski](https://github.com/ppkantorski)** — Creator of [**libultrahand**](https://github.com/ppkantorski/libultrahand) and [**sys-tune**](https://github.com/ppkantorski/sys-tune), powering the overlay framework and IPC architecture (`GPL-2.0-only`).
- **[dammitjeff](https://github.com/dammitjeff)** — Creator of [**Streamfin Switch**](https://github.com/dammitjeff/streamfin-switch), overlay UX reference.
- **[natinusala](https://github.com/natinusala)** — Creator of [**Borealis**](https://github.com/natinusala/borealis) (`Apache-2.0`).
- **[LRCLIB](https://lrclib.net/)** — Community-powered synchronized lyrics database.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [LICENSES/](LICENSES/) for full license texts.

---

## License

- The standalone application (`app/`) and background engine are licensed under **GPL-3.0-or-later** ([LICENSE](LICENSE)).
- The Ultrahand overlay (`phase2/overlay/`) is licensed under **GPL-2.0-only** ([LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt)).

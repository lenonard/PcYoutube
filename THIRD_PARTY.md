# Third-party components

PcYoutube Music v0.5 uses two runtime tools and two source dependencies fetched at build time.

## yt-dlp

- Project: https://github.com/yt-dlp/yt-dlp
- Packaged version: 2026.08.19
- Windows binary: `yt-dlp.exe`
- Purpose: YouTube keyword search, metadata extraction, source-format selection and direct audio URL resolution.
- License: Unlicense (see the upstream project for the authoritative license text).

## mpv

- Project: https://github.com/mpv-player/mpv
- Windows build project: https://github.com/zhongfly/mpv-winbuild
- Packaged build tag: `2026-09-29-b4b5d69a44`
- mpv commit: `b4b5d69a44e240e4a95c230bb7f018c381f0c5ae`
- Purpose: audio-only playback (`--no-video`) plus IPC transport, seek, volume, pause and realtime progress queries.
- License: mpv is distributed under GPL/LGPL terms depending on how it is built. Consult the upstream build and mpv repositories for the exact corresponding license/source information.

## Dear ImGui

- Project: https://github.com/ocornut/imgui
- Version/tag: `v1.92.9b-docking`
- Purpose: modern immediate-mode desktop interface rendered through the Win32 + Direct3D 11 backends.
- License: MIT.

## nlohmann/json

- Project: https://github.com/nlohmann/json
- Version/tag: `v3.12.0`
- Purpose: parse structured yt-dlp/mpv data and persist user playlists as JSON.
- License: MIT.

GitHub Actions fetches source dependencies during CMake configure and downloads the yt-dlp/mpv runtime binaries while creating the Windows artifact.

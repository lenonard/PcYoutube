# Third-party components

PcYoutube Music v0.7 uses three runtime tools and two source dependencies fetched at build time.

## yt-dlp

- Project: https://github.com/yt-dlp/yt-dlp
- Packaged version: `2026.08.19`
- Windows binary: `yt-dlp.exe`
- Purpose: YouTube keyword search, metadata extraction, source-format selection, direct audio URL resolution and fallback playback extraction.
- The official Windows executable includes the yt-dlp EJS component.
- License: Unlicense (see upstream for the authoritative license text).

## Deno

- Project: https://github.com/denoland/deno
- Packaged version: `v2.9.7`
- Windows binary: `deno.exe`
- Purpose: JavaScript runtime used by yt-dlp to solve current YouTube player challenges during full media extraction.
- License: MIT (see upstream for authoritative license/source information).

## mpv

- Project: https://github.com/mpv-player/mpv
- Windows build project: https://github.com/zhongfly/mpv-winbuild
- Packaged build tag: `2026-09-29-b4b5d69a44`
- mpv commit: `b4b5d69a44e240e4a95c230bb7f018c381f0c5ae`
- Purpose: audio-only playback (`--no-video`) plus IPC transport, seek, volume, pause, progress queries and yt-dlp fallback playback.
- License: mpv is distributed under GPL/LGPL terms depending on how it is built. Consult upstream for the exact corresponding license/source information.

## Dear ImGui

- Project: https://github.com/ocornut/imgui
- Version/tag: `v1.92.9b-docking`
- Purpose: immediate-mode desktop interface rendered through Win32 + Direct3D 11.
- License: MIT.

## nlohmann/json

- Project: https://github.com/nlohmann/json
- Version/tag: `v3.12.0`
- Purpose: parse yt-dlp/mpv data and persist playlist/settings JSON.
- License: MIT.

Windows-native WinHTTP and Windows Imaging Component are used for thumbnail download/decoding; they are operating-system components rather than bundled third-party libraries.

GitHub Actions downloads the pinned yt-dlp, Deno and mpv runtime binaries while creating the Windows artifact.

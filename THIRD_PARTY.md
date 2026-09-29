# Third-party components

PcYoutube Music v0.3 uses external command-line tools at runtime. They are not linked into the PcYoutube executable.

## yt-dlp

- Project: https://github.com/yt-dlp/yt-dlp
- Packaged version: 2026.08.19
- Windows binary: `yt-dlp.exe`
- Purpose: resolve a YouTube page/search result to an audio-only media URL and metadata.
- License: Unlicense (see the upstream project for the authoritative license text).

## mpv

- Project: https://github.com/mpv-player/mpv
- Windows build project: https://github.com/zhongfly/mpv-winbuild
- Packaged build tag: `2026-09-29-b4b5d69a44`
- mpv commit: `b4b5d69a44e240e4a95c230bb7f018c381f0c5ae`
- Purpose: audio playback only (`--no-video`) and IPC-controlled transport/volume.
- License: mpv is distributed under GPL/LGPL terms depending on how it is built. The packaged zhongfly build is redistributed as a separate executable; consult the upstream build and mpv repositories for the exact corresponding license/source information.

The GitHub Actions workflow downloads these binaries directly from their upstream release pages while creating the Windows artifact.

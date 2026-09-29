# PcYoutube Audio

Current prototype: **v0.3**.

PcYoutube Audio is a lightweight native C++ Windows player that does **not** use WebView. It resolves an audio-only media URL with `yt-dlp` and sends that direct URL to an `mpv` process running with video disabled.

## Playback flow

```text
YouTube URL / video ID / search text
        |
        v
      yt-dlp
  bestaudio[ext=m4a]/bestaudio
        |
        +--> title
        +--> direct media URL
                  |
                  v
             mpv --no-video
                  |
                  v
              speakers
```

The resolved media URL is displayed in the UI and can be copied to the clipboard. `Resolve + Play` always performs a fresh resolve because YouTube media URLs are temporary and normally expire.

For plain text input such as `Adele Hello`, PcYoutube uses `ytsearch1:Adele Hello` and resolves the first search result. A normal YouTube URL or 11-character video ID is also accepted.

## Player controls

- **Resolve + Play** - obtain a fresh audio URL and play it.
- **Pause / Resume** - toggle mpv pause state through its local named-pipe IPC interface.
- **Stop** - stop the current stream.
- **Volume** - native Windows slider controlling mpv volume.
- **Copy URL** - copy the currently resolved direct audio URL.

There is no browser surface, HTML rendering, YouTube video UI, or WebView dependency in v0.3.

## Runtime tools

The Windows package contains:

```text
out/
├─ PcYoutube.exe
├─ README.md
├─ THIRD_PARTY.md
└─ tools/
   ├─ yt-dlp.exe
   └─ mpv/
      └─ mpv.exe
```

The CI package currently pins:

- yt-dlp `2026.08.19`
- zhongfly mpv Windows build `2026-09-29-b4b5d69a44`

See `THIRD_PARTY.md` for upstream/source and license information.

## Architecture

```text
src/music/
├─ core/
│  ├─ music_app.h
│  └─ music_app.cpp
└─ platform/
   └─ windows/
      └─ main_win32.cpp
```

`pcyoutube_core` handles input normalization independently of the Windows UI. The Windows frontend handles process execution, yt-dlp output parsing, mpv IPC, clipboard operations, and native controls.

## Windows build

The C++ executable itself only needs MSVC, CMake, and Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
.\out\PcYoutube.exe --self-test
```

For normal playback, put `yt-dlp.exe` at `out\tools\yt-dlp.exe` and `mpv.exe` at `out\tools\mpv\mpv.exe`. GitHub Actions does this automatically for release artifacts.

## Notes

- Direct media URLs are temporary. Resolve again when one expires.
- Extraction can stop working when YouTube changes its delivery logic; updating yt-dlp is usually the first fix.
- Some videos may require authentication, region access, age verification, or other account/session context that this prototype does not import automatically.
- Use the application only with media you are permitted to access. This is an unofficial client and direct media extraction can be restricted by YouTube's terms or by content rights.

## CI

`.github/workflows/windows-cmake.yml` builds Windows x64 Release, runs CTest and the native self-test, downloads the pinned yt-dlp and mpv binaries from their upstream GitHub releases, and uploads the complete `out/` directory as `PcYoutube-audio-v0.3-windows-x64`.

# PcYoutube Music

Current prototype: **v0.5**.

PcYoutube Music is a lightweight native C++ Windows music player. It does not use WebView. Search and metadata are handled by `yt-dlp`; playback uses a direct media URL in an audio-only `mpv` process.

## What is new in v0.5

- persistent playlists stored in `%LOCALAPPDATA%\PcYoutube\playlists.json`;
- create/delete playlists, add/remove songs, and play directly from a playlist;
- Previous/Next follows the active search list or active playlist;
- fixed transport-bar layout so the large Play/Pause button keeps safe bottom padding;
- source-oriented quality presets: **Best source**, **Prefer Opus**, **Prefer AAC / M4A**, and **Data saver**;
- actual resolved codec, extension, source bitrate and sample rate are shown after resolving;
- custom Windows application icon plus version metadata embedded into `PcYoutube.exe`;
- `SHA256SUMS.txt` is included in CI artifacts for the main executable and bundled runtime tools.

## Search and playback

Enter a song, artist or album name to receive a list of results. A normal YouTube URL or 11-character video ID can also be pasted directly.

```text
Search / YouTube URL
        |
        v
      yt-dlp
  metadata + selected
  direct audio stream
        |
        v
   mpv --no-video
        |
        v
      speakers
```

The resolved direct URL is temporary. PcYoutube resolves it again whenever a song is selected from search or a playlist.

## Audio quality

PcYoutube v0.5 does **not** transcode audio. The selector chooses among formats that the source actually provides:

- **Best source** — best available audio-only stream;
- **Prefer Opus** — prefer an Opus/WebM source, then fall back to best available;
- **Prefer AAC / M4A** — prefer M4A/AAC, then fall back to best available;
- **Data saver** — prefer a source at or below roughly 64 kbps, with a low-quality fallback.

An MP3 label such as `320 kbps` would be misleading for direct streaming when YouTube does not provide a 320 kbps MP3 source. Converting a lower-bitrate AAC/Opus stream to MP3 320 kbps would only make a larger stream/file; it would not restore audio information that was not present in the source.

## Playlists

Open the **Playlists** tab in Library to create playlists. Search results and the currently playing song both have an **Add** / **Add to playlist** action.

Playlist data is stored outside the program folder at:

```text
%LOCALAPPDATA%\PcYoutube\playlists.json
```

This means replacing `PcYoutube.exe` with a newer build does not remove your playlists.

## Runtime package

```text
out/
├─ PcYoutube.exe
├─ README.md
├─ THIRD_PARTY.md
├─ SHA256SUMS.txt
└─ tools/
   ├─ yt-dlp.exe
   └─ mpv/
      └─ mpv.exe
```

The CI package currently pins:

- yt-dlp `2026.08.19`
- zhongfly mpv Windows build `2026-09-29-b4b5d69a44`

See `THIRD_PARTY.md` for upstream/source and license information.

## Windows SmartScreen / antivirus notes

The development builds are currently **not code-signed**. A newly generated unsigned executable has no publisher reputation, so Windows Defender SmartScreen can show **Windows protected your PC** / **Run anyway** even when the build is clean. Each unsigned release has a new file hash and must build reputation again.

The app also launches the bundled `yt-dlp.exe` and `mpv.exe`, communicates with mpv through a local named pipe, and opens temporary media URLs. Those behaviors can receive extra heuristic scrutiny from security products, but they are expected parts of the architecture.

For distribution, use a trusted code-signing certificate consistently or publish through a trusted store/channel. Self-signing alone does not establish public SmartScreen reputation. CI includes `SHA256SUMS.txt` so downloaded files can be compared with the build artifact hashes.

## Architecture

```text
src/music/
├─ core/
│  ├─ music_app.h
│  └─ music_app.cpp
└─ platform/windows/
   ├─ audio_backend.h
   ├─ audio_backend.cpp
   ├─ playlist_store.h
   ├─ playlist_store.cpp
   └─ main_win32.cpp
```

The UI is Dear ImGui + Direct3D 11. `audio_backend` owns yt-dlp/mpv process work and IPC. `playlist_store` owns persistent library data.

## Build

Using an MSVC developer environment with CMake and Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
.\out\PcYoutube.exe --self-test
```

For normal playback, put `yt-dlp.exe` at `out\tools\yt-dlp.exe` and `mpv.exe` at `out\tools\mpv\mpv.exe`. GitHub Actions does this automatically for release artifacts.

## Notes

- Direct media URLs are temporary and may expire.
- Extraction can stop working when YouTube changes delivery logic; updating yt-dlp is usually the first fix.
- Some videos can require authentication, region access, age verification, or other session context.
- Use the application only with media you are permitted to access.

## CI

`.github/workflows/windows-cmake.yml` builds Windows x64 Release, runs CTest and the native self-test, downloads pinned yt-dlp/mpv binaries, generates package checksums, and uploads the complete `out/` directory as `PcYoutube-Music-v0.5-windows-x64`.

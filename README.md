# PcYoutube Music

Current prototype: **v0.6**.

PcYoutube Music is a lightweight native C++ Windows music player. It does not use WebView. Search and metadata are handled by `yt-dlp`; playback uses a direct media URL in an audio-only `mpv` process.

## What is new in v0.6

- real YouTube thumbnails in Search, Playlists and Now Playing;
- thumbnails are downloaded asynchronously with WinHTTP and decoded with Windows Imaging Component, so the UI does not block on image loading;
- **Shuffle** mode for the active Search queue or Playlist queue;
- **Repeat Off / Repeat All / Repeat One** modes;
- automatic next-track playback when mpv reports the current song finished;
- manual Stop is distinguished from end-of-track, so Stop does not accidentally auto-advance;
- drag a playlist row by its `::` handle and drop it on another row to reorder tracks;
- persistent player preferences: volume, audio-quality preset, selected playlist, Shuffle and Repeat mode;
- previous behavior retained: persistent playlists, search results, direct URLs, source codec/bitrate display, seeking, volume and native application icon.

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

The resolved direct URL is temporary. PcYoutube resolves it again whenever a song is selected from Search or a Playlist.

## Thumbnails

For a YouTube video id, v0.6 requests the standard JPEG thumbnail from `i.ytimg.com` on a background worker. Windows Imaging Component decodes the JPEG directly into a D3D11 texture used by Dear ImGui.

This keeps the application native and avoids adding an embedded browser or a separate image-decoding framework.

## Audio quality

PcYoutube does **not** transcode audio. The selector chooses among formats that the source actually provides:

- **Best source** — best available audio-only stream;
- **Prefer Opus** — prefer an Opus/WebM source, then fall back to best available;
- **Prefer AAC / M4A** — prefer M4A/AAC, then fall back to best available;
- **Data saver** — prefer a low-bitrate source.

The actual resolved extension, codec, source bitrate and sample rate are shown in Now Playing when yt-dlp reports them.

## Playlists

Open the **Playlists** tab in Library to create playlists. Search results and the currently playing song both have an Add action.

Playlist data and player preferences are stored at:

```text
%LOCALAPPDATA%\PcYoutube\playlists.json
```

The file now contains both the playlist library and v0.6 settings. Existing v0.5 playlist files remain readable; missing settings simply use defaults.

Drag the `::` handle on a playlist row and drop it onto another row to change its position. The playing-track index is adjusted with the move so Previous/Next continues from the correct place.

## Playback modes

- **Shuffle** — Next/auto-next chooses another track from the active queue.
- **Repeat** — normal playback; playback stops after the final queue item.
- **Repeat ALL** — the final queue item wraps to the first.
- **Repeat ONE** — when a track finishes, the same resolved stream is played again.

When Previous is pressed more than four seconds into a song, it seeks to the start of the current song first.

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

The development builds are currently **not code-signed**. A newly generated unsigned executable has no publisher reputation, so Windows Defender SmartScreen can show **Windows protected your PC** / **Run anyway**. Each unsigned release has a new file hash and must build reputation again.

The app launches the bundled `yt-dlp.exe` and `mpv.exe`, communicates with mpv through a local named pipe, downloads thumbnails over HTTPS, and opens temporary media URLs. These are expected parts of the architecture but can receive extra heuristic scrutiny from security products.

CI includes `SHA256SUMS.txt` so downloaded files can be compared with the build artifact hashes.

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
   ├─ thumbnail_cache.h
   ├─ thumbnail_cache.cpp
   └─ main_win32.cpp
```

The UI is Dear ImGui + Direct3D 11. `audio_backend` owns yt-dlp/mpv process work and IPC. `playlist_store` owns persistent library/settings data. `thumbnail_cache` owns asynchronous HTTPS thumbnail download, WIC decode and D3D11 texture caching.

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

`.github/workflows/windows-cmake.yml` builds Windows x64 Release, runs CTest and the native self-test, downloads pinned yt-dlp/mpv binaries, generates package checksums, and uploads the complete `out/` directory as `PcYoutube-Music-v0.6-windows-x64`.

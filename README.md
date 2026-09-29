# PcYoutube Music

Current prototype: **v0.4**.

PcYoutube Music is a native C++ Windows audio player for YouTube media. It does not use WebView or an embedded browser. `yt-dlp` performs keyword search, metadata extraction and direct audio URL resolution; `mpv` handles audio-only playback.

## v0.4 highlights

- Modern dark Dear ImGui + Direct3D 11 interface with smooth vsync rendering.
- Real keyword search with a scrollable list of up to 16 YouTube results.
- Double-click or press **Play** on a result to resolve and start it.
- Large visual Play/Pause, Previous, Next and Stop transport controls.
- Realtime progress bar with current time, duration and seeking.
- Realtime pause/playback state and volume through mpv named-pipe IPC.
- Audio quality presets: **Best**, **High**, **Balanced** and **Data saver**.
- Expanded track information: title, channel, duration, requested quality, container, audio codec, bitrate, sample rate, format ID, source URL and direct audio URL.
- Direct YouTube URL and 11-character video ID input are still supported.

## Playback flow

```text
keyword / YouTube URL / video ID
             |
             v
           yt-dlp
     search + metadata
     quality selection
     direct audio URL
             |
             v
       mpv --no-video
             |
             v
          speakers
```

The media URL is resolved fresh for every selected song because YouTube delivery URLs are temporary.

## Audio quality presets

Quality is selected before opening a track:

- **Best** - `bestaudio`
- **High** - prefers audio streams at or above 160 kbps, with fallbacks.
- **Balanced** - prefers streams at or below roughly 128 kbps, with fallbacks.
- **Data saver** - `worstaudio`

The actual codec/bitrate/sample rate returned by yt-dlp is shown in the Now Playing panel when available.

## Runtime package

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

## Architecture

```text
src/music/
├─ core/
│  ├─ music_app.h
│  └─ music_app.cpp
└─ platform/
   └─ windows/
      ├─ audio_backend.h
      ├─ audio_backend.cpp
      └─ main_win32.cpp
```

`pcyoutube_core` contains portable input/search/quality helpers. `audio_backend` owns yt-dlp process execution, structured JSON parsing and mpv IPC. `main_win32.cpp` is only the Dear ImGui/D3D11 presentation and user interaction layer.

This separation keeps the project ready for a future Android frontend/backend without coupling the application logic to Win32 controls.

## Windows build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
.\out\PcYoutube.exe --self-test
```

CMake fetches Dear ImGui and nlohmann/json. For normal playback, place `yt-dlp.exe` at `out\tools\yt-dlp.exe` and `mpv.exe` at `out\tools\mpv\mpv.exe`; GitHub Actions packages them automatically.

## Notes

- Direct media URLs are temporary; select/play the song again to obtain a fresh URL.
- Extraction can stop working when YouTube changes delivery logic; updating yt-dlp is usually the first fix.
- Some media can require authentication, region access, age verification or other session context.
- This is an unofficial client. Use it only for media you are permitted to access and account for the applicable platform/content terms.

## CI

`.github/workflows/windows-cmake.yml` builds Windows x64 Release, runs CTest and the native self-test, bundles the pinned yt-dlp and mpv runtime tools, and uploads the complete `out/` package.

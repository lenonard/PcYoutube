# PcYoutube Music

PcYoutube Music is a lightweight native C++ desktop application with a music-first interface for browsing and playing YouTube content.

## Playback model

YouTube playback is handled through the official visible embedded player inside Microsoft Edge WebView2. The application does not extract, download, or separately decode YouTube audio streams.

You can:

- search YouTube by song, artist, album, or other keywords;
- paste a YouTube video URL or 11-character video ID;
- select a search result and play it inside the app;
- use the normal YouTube playback controls in the embedded player.

Some videos may refuse embedded playback according to the uploader's YouTube settings.

## Architecture

The application logic is separated from its platform frontend:

```text
src/music/
├─ core/
│  ├─ music_app.h
│  └─ music_app.cpp
└─ platform/
   └─ windows/
      └─ main_win32.cpp
```

`pcyoutube_core` contains platform-independent C++ code for YouTube video-ID parsing and URL construction. The Windows frontend is native Win32 plus WebView2.

This layout intentionally leaves the core reusable for a future Android frontend. An Android/NDK version can link the same core and provide an Android WebView-based platform layer.

## Windows requirements

- Windows 10 or Windows 11 x64
- Microsoft Edge WebView2 Runtime

Modern Windows installations commonly already include the WebView2 Runtime. If it is missing, the application reports that requirement in its status area.

## Windows build

Using an MSVC developer environment with CMake and Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
.\out\PcYoutube.exe
```

Run the non-interactive core smoke test with:

```powershell
.\out\PcYoutube.exe --self-test
```

## CI

`.github/workflows/windows-cmake.yml` builds Windows x64 Release on every push to `main`, verifies `out/PcYoutube.exe`, runs CTest and the native self-test, then uploads `out/` as the `PcYoutube-out-windows-x64` artifact.

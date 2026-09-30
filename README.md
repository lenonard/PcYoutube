# PcYoutube Music

Current prototype: **v0.7**.

PcYoutube Music is a lightweight native C++ Windows music player. It does not use WebView. Search and metadata are handled by `yt-dlp`; playback uses audio-only `mpv`.

## What is new in v0.7

v0.7 focuses on fixing the v0.6 packaging/playback failures:

- bundles the official `yt-dlp.exe` in the release package;
- bundles **Deno** beside yt-dlp because current yt-dlp requires a supported JavaScript runtime for full YouTube extraction;
- passes an explicit `--js-runtimes deno:<path>` to yt-dlp during full track resolution;
- preserves yt-dlp `http_headers` and sends them to mpv before direct-media playback;
- direct media URL remains the first playback path;
- if mpv is still idle after the direct stream attempt, the backend automatically retries the original YouTube URL through mpv's yt-dlp hook;
- runtime lookup is more tolerant: tools can be found in the packaged `tools/` directory, beside the EXE, or on PATH;
- release CI now performs a real YouTube resolve smoke test, not just keyword search/self-test;
- the artifact layout is flattened so `PcYoutube.exe` and `tools/` are directly visible after extraction.

All v0.6 features remain: native thumbnails, playlists, drag/drop ordering, Shuffle, Repeat, auto-next, quality selection, progress/seek, persistent settings and the Windows app icon.

## Why Deno is now included

Modern yt-dlp YouTube extraction uses external JavaScript challenge solving. Search with `--flat-playlist` can still work without a JavaScript runtime, while resolving a playable audio stream may fail. That matches the v0.6 symptom where search results appeared but pressing Play produced no audio.

v0.7 packages Deno in the same runtime folder and supplies its absolute path to yt-dlp.

## Runtime package

The v0.7 ZIP is intended to be extracted as one directory:

```text
PcYoutube.exe
README.md
THIRD_PARTY.md
SHA256SUMS.txt
tools/
├─ yt-dlp.exe
├─ deno.exe
└─ mpv/
   └─ mpv.exe
```

Do not move only `PcYoutube.exe`; keep the complete `tools` folder with it.

## Playback flow

```text
Search / YouTube URL
        |
        v
      yt-dlp
 + bundled Deno/EJS
        |
        +--> metadata
        +--> direct audio URL + HTTP headers
                         |
                         v
                    mpv --no-video
                         |
                  if direct fails
                         |
                         v
                mpv yt-dlp fallback
```

The direct URL is temporary and is freshly resolved whenever a track is selected.

## Audio quality

PcYoutube does not transcode audio. The selector chooses source formats:

- **Best source**
- **Prefer Opus**
- **Prefer AAC / M4A**
- **Data saver**

The actual resolved codec, bitrate and sample rate are shown when available.

## Persistent data

Playlists and player settings are stored at:

```text
%LOCALAPPDATA%\PcYoutube\playlists.json
```

Replacing the application package does not remove this file.

## CI verification

The Windows CI build performs:

- CMake/MSVC build;
- Windows version metadata check for `0.7.0`;
- CTest and native self-test;
- official yt-dlp download and version check;
- Deno download and version check;
- mpv packaging check;
- a real YouTube audio resolve using yt-dlp + the bundled Deno runtime;
- release-layout validation;
- SHA-256 generation for the main runtime executables.

## SmartScreen / antivirus

Development builds are not code-signed. SmartScreen may therefore show an unknown-publisher warning. Security software can also quarantine standalone tools such as yt-dlp. If PcYoutube reports that a runtime is missing after extraction, check the antivirus quarantine/history before manually downloading a replacement.

`SHA256SUMS.txt` is included so the packaged binaries can be checked against the CI artifact.

## Notes

- YouTube delivery behavior can change, so yt-dlp may need updates in future releases.
- Some media can require authentication, region access, age verification, or other session context.
- Use the application only with media you are permitted to access.

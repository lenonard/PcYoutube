# PcYoutube

Minimal native C++ desktop shell for PcYoutube.

The first sample lives under `src/helloworld` and keeps application/core code separate from the Windows frontend so another platform frontend (for example Android/NDK) can be added later without rewriting the core.

## Windows build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
.\out\PcYoutube.exe
```

A non-interactive smoke test is also available:

```powershell
.\out\PcYoutube.exe --self-test
```

## Layout

- `src/helloworld/core` - platform-independent C++ code.
- `src/helloworld/platform/windows` - lightweight native Win32/GDI GUI frontend.
- `.github/workflows/windows-cmake.yml` - Windows/MSVC CI build and artifact upload.

The current GUI intentionally avoids heavyweight frameworks and external runtime dependencies. An Android frontend can later link the same `pcyoutube_core` target from an NDK build.

## CI

Every push to `main` builds the Windows x64 Release target, runs CTest plus the native `--self-test`, and uploads the `out/` package as a GitHub Actions artifact.

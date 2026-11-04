# Godzilla for Windows

**[Version 1.0](https://github.com/DavyBirth/godzilla-pc/releases/tag/v1.0.0)** — native Windows runtime for Godzilla (PS4, 2015).

## Updating

Open the launcher, then **Updates → Check for updates → Update**. Existing saves, photos and settings are preserved. Earlier Preview launchers can update to 1.0.

The public ZIP updates an **existing prepared portable installation**. It does not prepare an original game dump automatically. Game files and console firmware are not distributed here; a complete owner-prepared portable folder is separate. No emulator or development tools are needed to run that prepared folder.

## Known limitations

Some buildings can look translucent in photo mode. A reported photo-mode entry failure in the bridge arena has not yet been confirmed resolved. Brief loading pauses were reported on a GTX and remain under investigation. VS is unavailable because PlayStation online services are not provided.

## Source and credits

Project source is available in this repository. The application ZIP also contains `SOURCE.zip`, including the pinned third-party dependencies. Licenses are included. Runtime based on [bbport](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4), GPL-2.0-or-later. Microsoft runtime libraries have their own license.

Godzilla ©2015 Bandai Namco Entertainment Inc. TM & © TOHO CO., LTD.
Independent project, not affiliated with or endorsed by the rights holders.

## Building from source

Windows 10/11 x64, LLVM/Clang, CMake 3.28+, Ninja, the Windows SDK and Vulkan SDK are required. Python 3 prepares the game image. Run from a developer terminal with the Windows SDK libraries available:

```powershell
git clone --recurse-submodules https://github.com/DavyBirth/godzilla-pc.git
cd godzilla-pc
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DGZ_DISTRIBUTION_BUILD=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake downloads the remaining dependencies at pinned revisions. Supply your own game data and firmware separately; building does not prepare a playable installation.

## Code layout

- `src/loader.c`: loads the prepared executable and connects its imports.
- `src/rt_*.c`: Windows implementations of the system services used by the game.
- `src/native_hook.*`: function replacement hooks.
- `src/pc_*`: controls, menus and photo mode.
- `src/*.cs`: launcher, translations and updater.
- `gpu/`: graphics integration; `gpu/vendor/` contains adapted upstream files.
- `scripts/`: preparation and validation of the owner's game files.
- `third_party/`: pinned bbport and shadPS4 source repositories.

Project C/C++ and C# use `.clang-format` (four spaces, 100 columns). Python scripts use Black with a 100-column limit. Keep upstream files and license notices intact.

## AAC decoder maintenance

Version 1.0 uses FFmpeg's native AAC decoder in place of FDK AAC. The build no longer fetches or links FDK AAC. The source package includes the complete FFmpeg 7.1.1 source and its pinned vcpkg build recipe, in addition to the prebuilt wrapper.

After building, run `powershell -NoProfile -File tools/test_ajm_aac.ps1` for isolated AAC checks. Fixtures are generated from a sine wave and silence; they contain no game recordings. `tools/prepare_ffmpeg_source.py` fetches and verifies the complete FFmpeg source for source packaging.

Existing Preview launchers can update directly to 1.0. The legacy `Godzilla-Windows-update.zip` asset is retained alongside the versioned ZIP; both contain the same package.

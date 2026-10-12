# Building Wingflight

Use Visual Studio 2022 with C++ tools and the Windows SDK, CMake 3.24 or later, and Git.

Clone with `--recurse-submodules`. If you download the source ZIP, run `python tools/fetch_dependencies.py` first. `dependencies.json` records the exact dependency versions and source links.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build/Release/Wingflight.dll`. This build targets Skyrim SE and AE, with VR disabled. Install the DLL with the matching Wingflight release files.

`scripts/` contains the two Papyrus helpers used by the mod. Compile them with Skyrim's Papyrus compiler and the game/SKSE script sources. WingflightAWUCompatibility also uses AWU's AFW_MagicEffect script source. CMake builds the DLL only.

The menu API header and its license are included in `vendor/SKSE-Menu-Framework-3-API`. Other dependency sources are pinned Git submodules. Copyright and license notices are in `LICENSE`, `COPYING`, `EXCEPTIONS.md` and `THIRD-PARTY-NOTICES`.

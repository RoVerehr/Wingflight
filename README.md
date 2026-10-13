# Wingflight 0.3.14

Wingflight adds flight with collisions, manual wingbeats and automatic hovering to Skyrim.

Default flight poses use the separately installed Flying Animations - Resource (FAR). Custom poses can be selected through Open Animation Replacer (OAR).

Maintained by ROSVER.

[Download on Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/194587)

To build from source:

```sh
git clone --recurse-submodules https://github.com/RoVerehr/Wingflight.git
cd Wingflight
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Requires Visual Studio 2022 with C++ tools and CMake. See [build notes](BUILDING-PUBLIC.md) for details.

Code is licensed under [GPL-3.0-or-later](COPYING) with the included [exceptions](EXCEPTIONS.md). You may use, modify and redistribute it under these terms.

Thanks to the authors of CommonLibSSE-NG, AWU, FAR, OAR and the other libraries used by Wingflight.

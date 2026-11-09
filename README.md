# Wingflight 0.4.5

A Skyrim SKSE mod with collision-aware flight, manual wingbeats and automatic hovering.

Per-wing appearance controls let the player set separate normal/flying sizes and vertical offset. Recognized SMP wings can use the same flight controller. Full flight can continue across scene loading.

Default flight poses use the separately installed Flying Animations - Resource (FAR); custom poses can be selected through Open Animation Replacer.

See [CHANGELOG.txt](CHANGELOG.txt) for release changes and [CUSTOM_WINGS.txt](CUSTOM_WINGS.txt) for custom wing configuration.

[Download on Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/194587)

Player settings are saved to `Data/SKSE/Plugins/Wingflight/Settings.ini`
(normally MO2 Overwrite). The first startup imports the installed `Wingflight.ini`;
later startups preserve player values, add missing settings and remove retired
settings using the current layout. Edit the player file for INI-only options,
then restart the game. Per-wing appearance is saved separately in
`Data/SKSE/Plugins/Wingflight/Appearance/Presets.ini`.
Both files are generated player data and should be excluded from release packages.

NPC Flight includes an optional Give Wings to Wingless Followers setting
(off by default), with an AWU wing-style selector. It creates temporary wings
only after the player enters full flight and only for followers without an
equipped/active wing source. This feature requires installed Animated Wings
Ultimate and its Wingflight providers; other flight features do not gain an
AWU dependency. Followers land before temporary wings are removed. Player wing
appearance settings do not change NPC wing size or position.

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

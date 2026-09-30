# Modern Physique Framework (MPF)

An SKSE plugin that applies named face morphs to Skyrim NPCs **at runtime**, on top of
whatever face is already loaded. No FaceGen is shipped and nothing is baked, so the mod
works over vanilla NPCs and over any NPC replacer without replacing anyone's work.

It is a framework, not a content mod. It ships no morphs and no NPC list: installed on
its own it changes nothing in the game. Content mods provide those.

## How it works

A content mod declares a physique, points it at a morph, and lists who gets it:

```ini
[Physique.Heavy]
FaceMorph     = RLY_Heavy
MalePresets   = MPP_Heavy      ; optional, assigned through the OBody NG API
FemalePresets = MPP_Heavy

[NPCs]
Alvor = 75, Heavy              ; 75% of the Heavy morph

[Factions]
BanditFaction = 100, Heavy, 30%
```

The weight is **how strongly the physique is applied**, not the vanilla fat slider. The
same number scales the face morph, which is what keeps a heavy face on a heavy body
without anyone authoring a second number.

### Matching a head

Morph sets are keyed by `(sex, vertex count)` and then confirmed against the live mesh's
own geometry — never by mod name. That separates COtR, High Poly COtR, High Poly Head,
Alternate High Poly Head and vanilla on its own, and a head nobody authored for is left
alone rather than morphed wrongly.

### Several content mods at once

Both the INIs under `Data\ModernPhysiqueFramework\` and the `.toml` manifests under
`Data\SKSE\Plugins\MPF\morphs\` are read as **directories**, not as single fixed files.
Each mod ships its own; conflicts are resolved by `Priority` (higher wins, ties broken by
filename) so the outcome never depends on directory or load order, and every decision is
written to the log.

## Layout

```
Data\
├── SKSE\Plugins\
│   ├── MPF.dll
│   └── MPF\morphs\           .tri files + one .toml manifest per content mod
└── ModernPhysiqueFramework\  one .ini per content mod
```

## Documentation

- [`docs/ModernPhysiqueFramework.schema.ini`](docs/ModernPhysiqueFramework.schema.ini) —
  every INI section, commented, with a working example
- [`docs/morphs.toml.example`](docs/morphs.toml.example) — the morph manifest format
- [`docs/MPF_ModPage.bbcode.txt`](docs/MPF_ModPage.bbcode.txt) — the Nexus page text

## Building

Requires CMake, a C++23 compiler and vcpkg.

```
cmake --preset build-release-msvc
cmake --build build/release-msvc
```

Dependencies come from vcpkg (`vcpkg.json`): CommonLibSSE-NG, tomlplusplus and SimpleIni.
`CMakePresets.json` points `CMAKE_TOOLCHAIN_FILE` at a local vcpkg checkout — change that
path to match your own.

Deployment is opt-in and needs an explicit destination, so a stale cache entry alone can
never copy a DLL into a game folder:

```
cmake --preset build-release-msvc -DRFM_ENABLE_POST_BUILD_DEPLOY=ON -DRFM_DEPLOY_OUTPUT=<path>
```

## License

[GPL-3.0-only](LICENSE).

`src/third_party/obody/API.h` is copied unchanged from
[OBody NG](https://github.com/Aietos/OBody-NG) 4.4.4 and is also GPL-3. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

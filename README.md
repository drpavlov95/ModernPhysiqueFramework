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
FaceMorph = RLY_Heavy
BodyMorph = RM_Heavy           ; RaceMenu slider embedded in the built TRI

[NPCs]
Alvor = 75, Heavy              ; 75% of the Heavy morph

[Factions]
BanditFaction = 100, Heavy, 30%
; BanditFaction = random, random, 30%  ; random strength and physique
```

The number is one shared physique percentage. MPF sends it to the named RaceMenu body
slider and matching face morph while preserving Skyrim's original NPC weight. Keeping
the engine's `_0`/`_1` interpolation untouched avoids a second, independent neck
deformation. RaceMenu body-morph `.tri` files are required for body changes; MPF writes
the named morph directly.

That distinction is important for compatibility: every nude body or outfit that can win
the asset conflict needs to contain the physique's named morph. A replacer that supplies
its own body path without a compatible `.tri` can leave the face morphed and the body
unchanged, which produces a neck seam. MPF cannot synthesize a missing mesh morph at
runtime.

### Two body routes for content mods

**BodySlide and runtime body morphs.** The content INI declares `BodyMorph`, users build
compatible bodies and outfits with BodySlide, and MPF asks RaceMenu to apply the named
morph at runtime. This is the flexible route: one built outfit can represent different
strengths. It does not use `outfits.ini`.

**Prebuilt meshes.** The content INI declares `FaceMorph` only. The content mod provides
ready-made bodies and outfits and can assign those assets to its NPCs. MPF still applies
the face morph at runtime, but there is no BodySlide step and no runtime body morph.

`Data\SKSE\Plugins\MPF\outfits.ini` is neither route. It is an optional generic fallback
table for content authors who want to map a vanilla default outfit to a replacement when
an MPF-managed NPC has no direct outfit assignment. Direct assignments take priority.
Physiques of Skyrim does not rely on this fallback: its Full package leaves the table
empty, while its Standalone package assigns its included prebuilt assets directly.

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

### Optional in-game configuration

With [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)
installed, MPF adds a `Modern Physique Framework / Configuration` page. It shows the
effective NPC list, the source mod for each entry, and a quiet conflict indicator. Opening
an NPC shows every INI that targets it and marks the active winner.

Editing or deleting an existing direct entry changes its source INI. A new NPC with no
existing source is written to `MPF_User.ini`; faction-generated entries can be turned into
an individual user override. The first edit to a file creates a `.mpf.bak`, writes are
performed through a temporary file, and changes are reloaded without restarting Skyrim.
The Content tab exposes source priorities and resolved physiques. Runtime logging and
manual refresh controls live under Settings; user logging is kept separately in
`Data\SKSE\Plugins\MPF\settings.ini` instead of modifying a content mod.

The menu framework is optional. Without it, MPF loads and applies content normally and
the F6 / Shift+F7 diagnostic controls remain available.

### Deterministic faction distribution

Faction rules can use `random` for physique, strength, or both. Results are deterministic
for each NPC record, so they survive reloads and cell changes. To intentionally reshuffle
only faction-generated choices, change `RandomSeed` in
`Data\SKSE\Plugins\MPF\settings.ini`. Explicit `[NPCs]` entries are unaffected.

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

The RaceMenu/SKEE BodyMorph declarations in `src/third_party/skee/SKEE.h` are adapted
from public interface headers. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

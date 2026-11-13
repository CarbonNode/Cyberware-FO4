# Cyberware for Fallout 4 (source)

Source code for **Cyberware - Ripperdoc Implant System**, an early-beta Fallout 4 mod: a ripperdoc who fits
implants, a clinic menu, arm weapons, abilities on hotkeys and a HUD.

**Mostly written with AI.** If that is a problem for you, this is not your mod.

## What is here, and what is not

Here:

- `framework/` - the F4SE plugin (C++23), its engine-free core, the generator that writes the plugin's records
  (C#), and a headless test of the core
- `ui/`, `hud/` - the clinic menu and the HUD, as Prisma UI views (HTML, CSS, JavaScript)
- `clinic/` - the generator for the clinic room and its layout data
- `tools/` - the catalogue builder and record scanners

Not here:

- **Anything from Cyberpunk 2077.** The mod uses assets converted from Cyberpunk 2077: arm meshes, textures,
  animations, sounds, item icons, and the item names, descriptions and numbers. None of that is in this
  repository, and neither are the tools that convert it. Cyberpunk 2077 and its content belong to
  CD PROJEKT RED. This is an unofficial fan work, not approved or endorsed by them.
- **So this repository does not build a working mod by itself, and the plugin does not compile as it stands.**
  `framework/plugin/main.cpp` includes two generated headers, `mantis_fold3p.inl` and `mantis_stow3p.inl`, that
  hold animation keys and measurements baked from the converted Mantis Blades animation and meshes. They are
  left out with the rest of the converted content. Without the catalogue the plugin would also have no
  implants to sell. `tools/build_catalog.py` is the builder, but the data it reads (`catalog_data.py`
  and the files beside it) is not here, so it does not run from this repository. The headless core test
  (`framework/test/cwtest.cpp`) compiles, but it needs a catalogue file to run and none is here either.
- **The test harnesses.** The in-game test plugin and the browser checks for the views are tied to the
  author's test setup and are left out. `framework/xmake.lua` still names a `CWHarness` target for the in-game
  one; build the `Cyberware` target only.

## Status

Early beta, 0.1.0. Fallout 4 1.10.163 only (the pre-next-gen build). What works, what is untested and what is
switched off is listed on the mod page.

## Building the plugin

These steps were read from `framework/xmake.lua`. They have **not** been run from a clean copy of this
repository, and they cannot finish from it: see the two missing generated headers above. This is cleaned
source, published to be read; it has not been build-tested in this form.

1. Windows, MSVC with C++23, [xmake](https://xmake.io) 3.0 or newer.
2. Check out the two libraries the build file expects (they are not in this repository):
   - `Dear-Modding-FO4/commonlibf4` at `a08c731eead4dfc6ea55aea4649c22aec05a47fe` into `framework/lib/commonlibf4`
   - `Dear-Modding-FO4/commonlib-shared` at `ed737c04811bf7c998f5a80340fd4ba5f9d3546b` into
     `framework/lib/commonlibf4/lib/commonlib-shared`
3. In `framework/`: `xmake f -m release -y` then `xmake build -y Cyberware`.

The record generator under `framework/espgen` needs the .NET SDK.

To run the mod: Fallout 4 1.10.163, F4SE 0.6.23, Address Library for F4SE Plugins, Prisma UI F4.

## Licence

GPL-3.0-or-later for the code and original art in this repository: see `LICENSE`.
`framework/third_party/nlohmann/json.hpp` is MIT, copyright its authors. `framework/plugin/PrismaUI_F4_API.h`
is Prisma UI's own API header, copied in as that header invites, copyright its authors. The fonts under
`ui/views` and `hud/views` are under the SIL Open Font License; each font folder carries the licence text.

# Credits and third-party notices

This project incorporates third-party code through the libraries listed below
and uses external format research. The format readers are written in C++ using
the references identified here. These credits distinguish linked libraries,
format knowledge and development tools.

## Libraries included in the DLL

- **MinHook 1.3.4**, by Tsuda Kageyu and contributors. Consumed through vcpkg
  and linked statically into `dinput8.dll` for runtime hooks in
  `resolver_probe.cpp` and `native_texture.cpp` (also used by the historical
  `asset_hook.cpp`).
  License: BSD-2-Clause. MinHook includes **Hacker Disassembler Engine**, by
  Vyacheslav Patkov, whose copyright and BSD notice are also preserved in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- **zlib 1.3.2#2**, by Jean-loup Gailly and Mark Adler. Consumed through vcpkg
  and linked statically into `dinput8.dll`. `package_dds_contract.cpp` uses it
  to decompress original package members for DDS and model validation; offline
  tests also use it. License: zlib. The installed package notice is reproduced
  in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Versions are recorded in the installed vcpkg metadata; the dependency baseline
is pinned in `vcpkg-configuration.json`.

## Format references used by the C++ implementation

- **[Darkstractor](https://github.com/BOTProT800/Darkstractor)**, by BOTProT800,
  MIT licensed (local `LICENSE`, copyright 2026). Its
  `darkstractor/formats/manifest.py` and
  `darkstractor/formats/deathinitive_obp.py` parsers were consulted as format
  references for manifest offsets, named OBPK members, type extensions, the
  first static asset anchors and the bounded streaming layout used by
  `darkstractor/formats/zlib_stream.py`.
  The implementations in `package_identity_catalog.cpp` and
  `package_dds_contract.cpp` are independent C++ expressions and do not invoke
  or redistribute Darkstractor.
- **[Darkside Mod Manager](https://github.com/BOTProT800/Darkside-Mod-Manager)**,
  by BOTProT800, MIT licensed (local `LICENSE`, copyright 2026). Its
  `darkside/formats/manifest.py` and `darkside/formats/obp.py` documentation and
  parser behavior were consulted to validate META records, the structural
  distinction between single-stream and per-member-block OBPK payloads, empty
  segments and the real package corpus. Its DDS/catalog checks remain an
  independent control; no Python source is shipped by this project.
- **[Anansi](https://github.com/BOTProT800/ANANSI)**, by BOTProT800,
  MIT licensed (local `LICENSE`, copyright 2026).
  `services/darksiders/mesh.py` was consulted for native Deathinitive `.2`
  metadata, static/skinned vertex blocks, index buffers and reference records.
  `model_validation.cpp` expresses that layout in C++ and adds strict validation
  for position-only edits. `services/darksiders/native.py` was also consulted
  for the verified normal/tangent offsets, duplicated normal Z and basis
  constraints used by the bounded-shape trial. `services/darksiders/animation.py`,
  Anansi's own ANM v1 reader, was consulted for the clip header, track records
  and compressed curves; `animation_validation.cpp` expresses that layout in C++
  for the same-structure `keys_in_place` contract. No Anansi executable or
  bundled converter is shipped.

The MIT notice shared by these three local source projects is reproduced in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Their external tools and
converters are not dependencies of this DLL. Darksiders2DLL itself, by the same
author, is also released under the MIT License from this version onward
(current code: 0.8.0-animation-trial); see [LICENSE](LICENSE).

## Earlier community research credited by those references

These are indirect sources, recorded in the local `CREDITS.md` files of
Darkstractor and Anansi. Their scripts and executables are not shipped here.

- **DS2extract**, the **ZenHAX manifest discussion** (`t=1738`, post `13332`),
  and the **`darksiders2.bms` / `darksiders.bms` QuickBMS scripts**: Darkstractor
  credits them for manifest/UPAK structure and the original OBPK name and folder
  tables. The historical script headers can be inspected in Darkstractor commit
  `289318a`. Those headers do not identify the script authors or licenses;
  these remain unverified. QuickBMS is credited there to **aluigi**.
- **finale00 (Tsukihime)**, for `fmt_DarkSiders2_dcm.py`: Anansi credits this
  Noesis plugin for the DCM header, mesh count tables and skinned position
  layout used as a starting point for its model reader.
- **zaramot**, for `DarkSiders_II_PC.ms`: Anansi credits this maxscript for
  independent DCM layout confirmation, skin weights and bone records.
- **Szkaradek123**, for the Blender 2.49 Darksiders II Deathinitive scripts:
  Anansi credits the `.2` importer for confirming the relationship to DCM and
  the one-byte mesh type field.
- **Doctor Loboto and other participants in the ZenHAX Deathinitive thread**
  (`t=8962`): Anansi credits the discussion for identifying the DCM relationship
  that guided its model-format research.

The local Anansi credits do not establish licenses for these community scripts
or forum material. Their mention here acknowledges format research and does
not assign them the MIT license of Anansi's own source.

## Build tools and other research references

- **Microsoft Windows SDK, DirectInput, MSVC, and vcpkg**, used to build and
  implement the `dinput8.dll` proxy. SHA-256 uses the Windows BCrypt API.
- **QuickBMS/offzip**, **DS2-RE**, **Darksiders-2-DLL-Loader**, **x64dbg**, and
  **Ghidra** are listed as research tools or possible references in
  `PLAN_MAESTRO.md`. That listing alone does not establish that their code,
  signatures or addresses were incorporated. Any such incorporation must
  record its exact origin and license here.

The release packaging script includes both this file and
`THIRD_PARTY_NOTICES.md` in the distributable ZIP.

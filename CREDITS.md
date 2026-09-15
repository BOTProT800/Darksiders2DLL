# Credits and third-party notices

This project uses or is informed by the following software and research. No
third-party source code has been copied verbatim into this repository; the
format knowledge identified below is re-expressed in the project's own C++.

- **MinHook**, by Tsuda Kageyu and contributors. MinHook is consumed through
  vcpkg and remains subject to its upstream 2-clause BSD license. Its license
  file is installed alongside the vcpkg package metadata and reproduced in
  `THIRD_PARTY_NOTICES.md` for binary redistribution.
- **zlib 1.3.2#2**, by Jean-loup Gailly and Mark Adler. The original
  DDS contract extractor in Debug and Release and its offline tests consume zlib through vcpkg.
  zlib remains subject to the zlib license; the installed package notice is
  reproduced in `THIRD_PARTY_NOTICES.md` for binary redistribution.
- **Microsoft Windows SDK, DirectInput, MSVC, and vcpkg**, used to build and
  implement the `dinput8.dll` proxy.
- **[Darkstractor](https://github.com/BOTProT800/Darkstractor)**, by BOTProT800,
  MIT licensed (local `LICENSE`, copyright 2026). Its `formats/manifest.py` and
  `formats/deathinitive_obp.py` parsers were consulted as format references for
  manifest offsets, named OBPK members, type extensions, the first static asset
  anchors and the bounded streaming layout used by `formats/zlib_stream.py`.
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
- **Anansi**, a local MIT-licensed project reserved as a format reference for
  future model-loading phases.
- **QuickBMS/offzip**, the community Darksiders scripts, DS2-RE,
  Darksiders-2-DLL-Loader, x64dbg, and Ghidra are research/tooling references
  identified by the project plan. Any future copied or adapted implementation
  must add its exact origin and license here before distribution.

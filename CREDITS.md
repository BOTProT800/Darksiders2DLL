# Credits and third-party notices

This project uses or is informed by the following software and research. No
third-party source code has been copied into this repository at this stage.

- **MinHook**, by Tsuda Kageyu and contributors. MinHook is consumed through
  vcpkg and remains subject to its upstream 2-clause BSD license. Its license
  file is installed alongside the vcpkg package metadata and reproduced in
  `THIRD_PARTY_NOTICES.md` for binary redistribution.
- **Microsoft Windows SDK, DirectInput, MSVC, and vcpkg**, used to build and
  implement the `dinput8.dll` proxy.
- **Darkstractor** and **DarksideModManager**, local MIT-licensed projects used
  as format and validation references for later asset-loading phases.
- **Anansi**, a local MIT-licensed project reserved as a format reference for
  future model-loading phases.
- **QuickBMS/offzip**, the community Darksiders scripts, DS2-RE,
  Darksiders-2-DLL-Loader, x64dbg, and Ghidra are research/tooling references
  identified by the project plan. Any future copied or adapted implementation
  must add its exact origin and license here before distribution.

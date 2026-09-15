# Rebuild verification — 2026-09-15

The reconstructed source tree was rebuilt from a clean directory with the preserved reference toolchain supplied by the project owner.

## Toolchain

- LLVM-MinGW UCRT 2026-08-26 x86_64
- Clang 23.1.0
- Generator: Ninja
- Build type: Release
- Target: Windows x86-64 PE32+
- Toolchain archive SHA-256: `cee8d2ce3da5145ce4dc882e70d0b0719a783d53a99752c60948fc0659975a65`

## Clean build result

CMake configuration completed successfully. Ninja completed all **413/413** compile/link steps and ended with:

`Linking CXX executable bin/openxcom.exe`

Fresh normal/dynamic executable:

- size: **19,397,120 bytes**
- SHA-256: `7dcd243a27e9592e3ff7e5e52a5a4257c7549558ed2a36849c3f1e05a42b3039`

Historical validated `AUTONOME` reference executable:

- size: **25,474,048 bytes**
- SHA-256: `4a4d49c81502556f46486dd6cbdd4a0d465a26c2e580158470baca657ca9d93a`

## Byte-for-byte comparison

The two executables are **not byte-identical**.

The reference `AUTONOME` build statically contains SDL/media code and does not import:

- `SDL.dll`
- `SDL_gfx.dll`
- `SDL_image.dll`
- `SDL_mixer.dll`

The normal repository build uses the bundled import libraries and imports those DLLs. This changes `.text`, `.rdata`, `.data`, unwind/relocation layout, file size and hash. The historical standalone/static SDL/media archives and exact standalone link recipe are not preserved in this repository.

Both executables are AMD64 PE32+ images with 15 sections, the same entry point (`0x13A0`), image base (`0x140000000`), section alignment (4096), file alignment (512), Windows console subsystem, and the same `.rsrc` virtual size (`0xB570`).

## Application-level source equivalence check

Using LLVM's COFF symbol reader, both binaries contain exactly the same set of **23,283 unique demangled symbols containing `OpenXcom::`**:

- only in the AUTONOME reference: **0**
- only in the fresh repository build: **0**

Critical HD/D3D11 runtime marker strings are also present in both builds, including:

- `RC12 P10A ENV A/B TOGGLE`
- `[HD-PERF RC12-P10 NATIVE ENV GRADE]`
- `[RC12 P10 GPU] D3D11 indexed/native-environment/fixed compositor ready`

## Conclusion

The reconstructed repository is **successfully buildable with the exact historical LLVM-MinGW toolchain** and reproduces the complete OpenXcom/OXCE application symbol surface of the validated AUTONOME executable.

It does **not** reproduce the historical AUTONOME file byte-for-byte because that file used a different standalone/static dependency packaging step. Reproducing that exact file would additionally require the exact static SDL/media archives and standalone link recipe.

This verification is a compile/link and binary-structure comparison. The fresh executable was subsequently smoke-tested successfully on Windows
by the project owner, including verification of the current tactical menus
and recent engine changes, and was then published as the Windows x64 build
for the v0.1.0 pre-release.

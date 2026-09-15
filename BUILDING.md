# Building the Windows x86-64 executable

## Reference toolchain

The validated RC lineage was built with:

- Base OXCE source: `22f1aae75c3047ceb0c0e24abe74938fa7230793`
- Target: Windows x86-64 PE32+
- Toolchain: LLVM-MinGW UCRT 2026-08-26 x86_64
- Generator: Ninja
- Build type: Release

The repository includes `toolchain-llvm-mingw-x64.cmake`, which was used by the development lineage. Its `TOOLROOT` contains the original build-machine path; change that one path to the location where the matching LLVM-MinGW toolchain is installed.

## Reference commands

From the repository root:

```sh
cmake -S . -B build-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-llvm-mingw-x64.cmake

cmake --build build-release -- -j3
```

The historical build notes identify `ninja -j3` as the final build command used in the validated Windows lineage.

## Verified rebuild

On 2026-09-15 this reconstructed source tree was configured and compiled successfully from a clean build directory with the preserved **LLVM-MinGW UCRT 2026-08-26 x86_64** toolchain (Clang 23.1.0). Ninja completed all **413/413** Windows x86-64 Release build/link steps and produced `bin/openxcom.exe`.

The normal repository build uses the bundled SDL import libraries and therefore produces a DLL-based executable. The historical validated executable carrying the `AUTONOME` suffix used statically linked SDL/media dependencies, so the two binaries are not byte-identical even though the OpenXcom application symbol surface matches exactly. See `REBUILD_VERIFICATION.md`.

For a public binary release:

1. tag the exact source state;
2. build from that tag with the reference toolchain;
3. smoke-test the resulting executable on Windows with the required runtime DLLs;
4. attach that tested executable (and DLL package if using the normal dynamic build) to the GitHub Release for the same tag.

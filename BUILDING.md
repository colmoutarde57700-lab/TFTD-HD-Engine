# Build P2ZJ on Windows x64

Reference tools: LLVM-MinGW UCRT 2026-08-26 (Clang 23.1.0), CMake and Ninja. Put CMake and Ninja on PATH. SDL import libraries are tracked in deps/lib/x64. Runtime DLLs are distributed in the Windows release package, not tracked in Git. Copy those DLLs into deps/lib/x64 before building, or beside the resulting executable before running it.

From the repository root in PowerShell:

```powershell
git checkout v0.2.0
$env:LLVM_MINGW_ROOT = 'C:/Tools/llvm-mingw-20260826-ucrt-x86_64'
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain-llvm-mingw-x64.cmake" -DBUILD_PACKAGE=ON -DDEV_BUILD=ON -DBUILD_HD_CONTRACT_TESTS=ON
cmake --build build-release --parallel 8
ctest --test-dir build-release --output-on-failure
python tests/validate_shaders.py --output build-release/shader-validation
```

HLSL validation needs Windows d3dcompiler_47.dll. Resource paths are relative to the source root. Keep copied DLLs beside build-release/bin/openxcom.exe. The Windows release package supplies those DLLs for a fresh source checkout.

real_hd_pixel_firewall is required on each engine build. BUILD_HD_CONTRACT_TESTS enables the delivered P2ZJ storey/caustic regression without game data or private art. Build metadata records the exact source commit and hashes. Bit-for-bit identity across tool versions, directories or Git metadata, or with the historical private executable, is not promised.

# Source provenance

## Upstream

This project is based on OpenXcom Extended / OpenXcom source snapshot:

`22f1aae75c3047ceb0c0e24abe74938fa7230793`

## Current stable source state

The current tree was reconstructed from the preserved full RC12 source lineage and the preserved source deltas leading to the validated stable executable.

The important stable milestones are:

`OXCE 22f1aae…` → `RC12 P9A` → `P10 Native Environment Grade` → `P10A Environment A/B` → later validated tactical/HUD/lighting/flare fixes → `HUD_CONTACT_COLUMNS_V1`.

Experimental branches, rejected diagnostics, temporary tests and rollbacks are intentionally **not** included in this public repository. The repository stores the resulting stable source state directly.

The complete reconstruction audit and historical patch archive are retained separately by the project owner.

## Validated reference executable

The preserved stable reference executable used to identify this source lineage is:

`OpenXcomExtended_TFTD_HD_RC12_P10A_PSI75_MC2X2_HUD_VISIBLE_RIGHT_ANCHOR_LIGHTING_V1_FLARE_RECON_V1_FLARE_DECAY_V1_HUD_CONTACT_COLUMNS_V1_AUTONOME.exe`

SHA-256:

`4a4d49c81502556f46486dd6cbdd4a0d465a26c2e580158470baca657ca9d93a`

The source reconstruction chain applied cleanly.

On 2026-09-15, the reconstructed source tree was rebuilt from a clean
directory using the preserved LLVM-MinGW UCRT 2026-08-26 x86_64
toolchain (Clang 23.1.0). Ninja completed all 413/413 Windows x86-64
Release compile/link steps successfully.

The resulting repository build is not byte-identical to the historical
AUTONOME reference executable because the repository build uses DLL-based
SDL/media dependencies while the AUTONOME build used static SDL/media linkage.

Application-level comparison found the exact same set of 23,283 unique
OpenXcom:: symbols in both executables.

The clean rebuild was subsequently smoke-tested successfully on Windows
by the project owner and became the Windows x64 binary distributed with
the v0.1.0 pre-release.

## Asset boundary

A comparison against the upstream source snapshot found no newly added commercial TFTD image/audio/game-data files in the modification set. The private HD art/assets and original TFTD game resources are not part of this repository.

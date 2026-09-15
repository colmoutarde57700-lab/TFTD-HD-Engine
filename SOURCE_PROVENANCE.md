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

The source reconstruction chain applied cleanly. The current packaging environment does not contain the exact LLVM-MinGW 2026-08-26 toolchain, so a fresh byte-for-byte Windows rebuild was not performed while preparing this repository package.

## Asset boundary

A comparison against the upstream source snapshot found no newly added commercial TFTD image/audio/game-data files in the modification set. The private HD art/assets and original TFTD game resources are not part of this repository.

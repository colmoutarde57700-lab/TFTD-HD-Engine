# TFTD HD Engine — modified OpenXcom Extended

This repository contains the source code for a modified build of **OpenXcom Extended (OXCE)** used by the private **TFTD HD** rendering/remaster project.

## Upstream base

- Project: OpenXcom Extended / OpenXcom
- Upstream source snapshot: `22f1aae75c3047ceb0c0e24abe74938fa7230793`
- License: GNU GPL v3 (see `LICENSE.txt`)
- Modified source snapshot prepared: 2026-09-15

This is a modified version. It is not an official OXCE release.

## What this fork changes

The current source tree contains the HD/rendering and tactical changes used by the validated RC12 branch, including the Direct3D 11 HD compositor, x16 HD render-space support, true-colour RGBA handling alongside Legacy indexed graphics, HD environment colour grading, high-precision frame pacing, tactical HD asset routing, later tactical fixes, lighting/flare work and the current HUD contact-column implementation.

See `MODIFICATIONS.md` for a structured summary and `SOURCE_PROVENANCE.md` for the source lineage.

## Game data is not included

This repository **does not contain the commercial X-COM: Terror From the Deep game data** and does not contain the private TFTD HD art/assets.

A legally obtained copy of the original game is required, in the same way that OpenXcom/OXCE requires original X-COM game resources.

The `bin/UFO` and `bin/TFTD` folders contain only the upstream placeholder/readme material from the OXCE source tree. Do not commit copied Steam/original game resources into this repository.

## Building

The validated Windows lineage used:

- Windows x86-64 target
- LLVM-MinGW UCRT 2026-08-26
- CMake
- Ninja
- Release build

See `BUILDING.md`.

## License

The OXCE/OpenXcom-derived code remains licensed under the **GNU General Public License v3**. Existing copyright and license notices have been preserved.

This repository contains source code only. Binary releases, when published, should be attached to a GitHub Release whose tag points to the matching source state.

For the original upstream project documentation, see `README_UPSTREAM.md`.

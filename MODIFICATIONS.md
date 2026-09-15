# TFTD HD engine modifications

This file summarizes the principal modifications relative to upstream OXCE commit `22f1aae75c3047ceb0c0e24abe74938fa7230793`.

## Rendering / HD architecture

- HD render-space work through x16 / 512×640 tactical-cell assets.
- Coexistence of Legacy indexed graphics and true-colour RGBA assets.
- Direct3D 11 HD GPU compositor/backend.
- HD image loading/cache and HD visual routing rules.
- Separate HD colour semantics for Legacy indexed, environment-graded and fixed/native artwork.
- Continuous native RGB environment transform with CPU/GPU paths.
- High-precision frame pacing while keeping gameplay timing separate.
- Independent HD presentation timing where applicable, while later restoring exact Legacy cadence for automatically discovered terrain animations.

## Tactical / compatibility work in the current stable lineage

The final stable lineage incorporates, in order, the preserved P10A baseline and subsequent work covering:

- Legacy terrain-animation cadence compatibility.
- Environment family/block profiling used by the HD terrain path.
- Training visual roster and safe playable alien test support.
- Native alien weapons / mind-control related training fixes.
- Inventory safety fixes for test units.
- Unit-part rendering integration.
- Faction/equipment audit fixes.
- Debug-turn decoupling and animation timing audit.
- PSI cadence adjustment.
- Multi-tile mind-control/self-indicator correction.
- HUD visible-unit right anchoring.
- Lighting V1 changes.
- Flare reconnaissance rendering.
- Chemical flare decay/burn-cycle handling.
- Current HUD contact-column layout.

## Exact modified-file inventory

See `docs/MODIFIED_FILES_FROM_UPSTREAM.txt` for the automatically generated list of code/data files that differ from the upstream snapshot before the public documentation files were added.

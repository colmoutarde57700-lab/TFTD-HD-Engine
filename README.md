# TFTD HD Engine — P2ZJ

Modified OpenXcom Extended for the TFTD REAL HD project. This development prerelease follows public RC12 and imports the current P2ZJ engine source snapshot, dated 26 September 2026.

Upstream base: `22f1aae75c3047ceb0c0e24abe74938fa7230793`. GNU GPL v3 (LICENSE.txt). This is an unofficial OXCE fork.

## Download and installation

Get the Windows x64 package, matching source archive, build record and SHA-256 checksums from [release v0.2.0 — P2ZJ](https://github.com/colmoutarde57700-lab/TFTD-HD-Engine/releases/tag/v0.2.0). The Windows ZIP includes openxcom.exe, its runtime DLLs and upstream common/standard resources. Extract it into a separate folder or back up your existing executable and DLLs before replacing them. Supply your original TFTD data and compatible HD content separately.

The engine retains the upstream `-version` text **Extended 8.6 (v2026-04-26)**. Identify this TFTD HD release by its package name, source commit and SHA-256 from BUILD_RECORD.json; that upstream text does not identify the P2ZJ release.

REAL HD uses Direct3D 11 with separate HD scene, visibility, lighting and presentation authorities. P2ZJ adds storey-band composition for staircases and sunlight-exposure checks for caustics below roofs. See MODIFICATIONS.md and SOURCE_PROVENANCE.md.

Commercial TFTD data and private TFTD HD artwork are excluded. Supply legally obtained original game data and compatible HD content. bin/TFTD and bin/UFO contain upstream readme placeholders only. The separate Workshop application is not included.

BUILDING.md gives build/test commands. REBUILD_VERIFICATION.md explains their limits. Prior owner confirmation concerns staircase and roof-caustic corrections; broad mission, UI and performance validation remains incomplete. Public binaries must be built from the recorded release source commit, following RELEASE_PROCESS.md. RC12 records are preserved under docs/release-history/rc12.

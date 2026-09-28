# v0.2.0 — P2ZJ REAL HD development prerelease

Advances public RC12 to P2ZJ: REAL HD scene/visibility/lighting/presentation migration, stair storey composition and roof-aware caustics.

Windows x64 binary freshly built from the recorded source commit; artifacts identified in SHA256SUMS.txt. Extract in a separate folder or back up the existing EXE/DLLs, then supply legally obtained TFTD data and compatible HD content. Commercial data, private HD art and Workshop are excluded.

Checks cover geometry/composition contracts, mandatory pixel guards and nine HLSL entry points; BUILD_RECORD.json gives results and diagnostics. Stair/roof-caustic fixes have prior owner confirmation in game; this rebuild has no new mission acceptance. Full UI/mission coverage and performance remain incomplete. Existing HLSL diagnostics include integer-division precision and potentially uninitialized roofCaustic; successful shader compilation does not dismiss those warnings.

Unofficial development prerelease. Upstream OpenXcom Extended/OpenXcom, GNU GPL v3.

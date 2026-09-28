# P2ZJ source provenance

Upstream base: `22f1aae75c3047ceb0c0e24abe74938fa7230793`. Public baseline: main at `63b4b10`, RC12 v0.1.0 / v0.1.1 (documentation correction only). RC12 records are retained under docs/release-history/rc12.

New authority: SOURCE from TFTD_P2ZJ_ESCALIERS_CAUSTIQUES_TEST, dated 26 September 2026, continuing P2ZI and adding P2ZJ stair/roof-caustic fixes. Application code, build modules, libraries and upstream resources are imported. Private notes, commercial data and HD art are excluded.

Historical installed/delivered P2ZJ EXE: 21,184,512 bytes, SHA256 `d5ff1f5fa17530063566396ff721f7c078180981ccbfe2856082ee5a7c9b83aa`. This is the provenance reference only; the public binary is freshly built and has its own checksum.

Packaging adaptations remove absolute resource paths, add the source-root resource include, configure LLVM_MINGW_ROOT and integrate the delivered regression/HLSL checks. P2ZJ runtime C++ logic is preserved. The release record identifies actual source commit, build and tests separately from prior owner confirmation of stair/roof-caustic behavior. Full mission/UI acceptance and performance remain incomplete.

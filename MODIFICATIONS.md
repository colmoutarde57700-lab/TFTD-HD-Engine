# Changes since public RC12

## REAL HD migration through P2ZI

Renderer-neutral physical geometry, scene construction, world coverage, visibility/FOV, lighting/perception authority and HD presentation. The required pixel firewall guards native OXCE raster presentation paths. GPU rendering, providers, interface production, camera and combat effects evolved with this migration.

## P2ZJ stairs

Disjoint storey bands prevent composition gaps and overlaps. Own-floor and virtual stair positioning are handled separately. 3D replacement marks only the actual command range, preserving unrelated earlier commands. This does not claim universal 3D depth correctness for every scene.

## P2ZJ roof caustics

Upward-facing floor caustics depend on sunlight exposure. BlockLight checks across all higher storeys use a 16-by-16 grid and respect partial coverage. Sight-only barriers do not block light; a roof does not occlude itself.

## Release preparation

Portable resource/toolchain paths, source/binary provenance and automated geometry, pixel-firewall and HLSL checks. RC12 history retained. Commercial data, private art and Workshop excluded.

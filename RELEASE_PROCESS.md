# Release process

For every public executable:

1. Commit the final stable source state.
2. Create a Git tag for that exact state (for example `v0.1.0`).
3. Compile from that tagged source.
4. Test the executable.
5. Create a GitHub Release from the same tag.
6. Attach the executable and a SHA-256 checksum file to the Release.

Do not put original TFTD/Steam game data or private HD art assets into this engine repository.

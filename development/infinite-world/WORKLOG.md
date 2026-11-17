# Infinite-world engineering worklog

This is an internal engineering notebook for the ClassiCube infinite-world implementation. It records durable findings so future work can resume without repeating whole-subsystem audits.

Scope: singleplayer infinite worlds. Finite worlds and existing .cw compatibility must remain unchanged. Do not edit README as part of this work unless explicitly requested.

## Current state

- Infinite-world mode is integrated into the launcher/world lifecycle.
- The infinite-world store, deterministic terrain generation, chunk streaming/rendering, block editing, physics, and ICW save/load paths are implemented.
- Most major subsystems have received a source-level audit (see below).
- **The end-to-end playable milestone is not yet verified.** The required milestone is: create an infinite world, spawn, walk continuously in any direction, and see terrain stream without gaps or crashes.
- Do not describe the project as finished until the playable milestone and persistence/finite-world regression checks are completed.

## Immediate next work

1. Continue the mesh-geometry and GPU-buffer audit from the latest master.
2. Trace builder vertex counts and allocation limits through every graphics backend.
3. Review the latest index-safe draw batching change (commit listed below), especially batch boundaries, GL state restoration, D3D draw arguments, and large vertex-buffer offsets.
4. Reach and verify the first playable streaming milestone.
5. Verify save → quit → reload preserves edits and world identity.
6. Regression-check finite worlds and .cw loading/saving.

## Recent implementation commits

- `c86b2839ad05ebc0a3aab67a78246960ab0652a4` — Split oversized chunk draws into index-safe batches. Changes GL1, GL2, D3D9, and D3D11 draw paths so a draw is divided into batches no larger than the shared 16-bit index buffer's 65,536-vertex addressable range. **Review this change as part of the current graphics audit; do not assume the diff alone proves all backends correct.**
- `6158620257c3d3493325977499cad793604132e0` — Handle GL11 chunk display-list allocation failures.
- `f245db445647fbf88357e0dd88e5a85b884fcae6` — Clear stale chunk part metadata on empty batches.
- `7ff66de5d2ea08b5f281db449d1ab0b4adee4c99` — Report unsupported extended IDs in standard builds.
- `0b4990942626e06f56630161e8d78bbfd4044eea` — Skip terrain generation when loading saved chunks.
- `d4af7a2bef6730a9cb9beb1bab6defc64cae8152` — Keep infinite saves within seekable file limits.
- `bd8037129893dda7e28f584e1103498737488eb6` — Preflight infinite save limits before creating files.
- `06e238ccf575a1802d92f48ae271599eec89c82a` — Make terrain interpolation portable for negative noise.
- `64b5314d5a5158991d7257eb1411449484eef9a6` — Avoid signed overflow in infinite sprite placement seeds.
- `f90d89d8a04a77d03dbff30c4e79da42799efd1d` — Skip finite side walls in infinite-world block picking.

## Audited areas and conclusions

These are prior source-audit findings, not substitutes for runtime verification.

### Chunk storage, eviction, and persistence
- Unmodified chunks may be evicted and regenerated from the seed.
- Modified chunks persist as sparse differences from generated terrain when compression succeeds.
- Dense modified chunks that cannot be compressed remain resident; the nominal resident target is therefore soft to avoid losing edits.
- Sparse chunks are reconstructed from generated terrain plus saved differences.
- Chunk compaction rebuilds indexes/LRU state and retains the previous structures if rebuilding fails.
- ICW2 stores 16-bit block IDs; load uses staged data and does not replace the active world until validation/loading succeeds.
- Loading saved chunks uses a non-generating allocation path, avoiding unnecessary terrain generation.
- Save-size cap is 65,520 records because file seek/position APIs are 32-bit constrained.
- Save preflight checks limits before creating the destination file.
- Known limitation: save writes directly to the final path. A failed partial write can leave a damaged file; the current portable file API does not provide a safe atomic temp-and-replace workflow.

### Terrain determinism and boundaries
- Generation is keyed by absolute coordinates and deterministic hashes.
- Negative chunk coordinates use mathematical floor division.
- Tree anchors include a halo for cross-chunk canopy consistency.
- Ore/cave sampling includes neighbouring grid centres/margins to avoid seams.
- Signed-shift and signed-overflow hazards in terrain/sprite seed calculations were fixed in the commits above.
- Vertical world range is Y=[0,64); below is bedrock and above is air.

### Rendering and chunk invalidation
- Infinite renderer uses a reusable 21×21 horizontal ring with four vertical layers (1,764 render slots); this is a render window, not the full persistent world.
- Horizontal edge/corner and vertical boundary edits invalidate affected neighbouring meshes.
- Ring slots are checked against their world coordinates before reuse.
- Empty meshes become dirty/non-empty when a solid block is added.
- Failed mesh builds remain dirty and retry with adaptive delays.
- Chunk builds are prioritised by camera distance and constrained by an adaptive per-frame budget.
- Actual dynamic memory pressure is dominated by generated vertex data/GPU buffers, not the fixed per-slot metadata.
- GL1 display-list/buffer allocation failure handling and stale empty-part metadata were addressed in recent commits.

### Lighting
- Infinite worlds currently force Classic lighting.
- Fancy lighting relies on finite-world dimensions/heightmap indexing and is not safe to enable as-is.
- Proper infinite Fancy lighting would require chunk-local light storage, cross-chunk propagation, persistence/eviction policy, and mesh invalidation.
- This remains a known feature limitation, not a patch to make casually.

### Physics and block interaction
- Collision checks fail safely when terrain data is unavailable instead of treating missing chunks as air.
- Liquid spread requeues sources when required neighbouring data is unavailable.
- Sponge operations preflight their full affected volume before mutation.
- Falling blocks check destination data before moving.
- Physics queue is capped at 65,536; queue saturation/allocation failure can defer/drop updates, with random-tick rediscovery recovering many liquid cases over time.
- Physics queue state is transient and is not serialized; persistent block state is saved.

### Coordinates and scale
- Generation uses 64-bit intermediates for coordinate calculations and excludes outer chunk coordinates where mesh halos exceed signed block-coordinate bounds.
- Camera/entity positions still use 32-bit floats. At very large distances, adjacent block/chunk positions lose precision. A proper fix requires a floating origin/rebased coordinate design across movement, rendering, picking, and entities; no isolated patch has been identified.

### World lifecycle and launcher
- Launcher offers separate finite/infinite world choices.
- Infinite mode is selected through a one-shot startup option; dropped finite .cw maps retain precedence.
- World transitions reset the previous finite/infinite storage through the normal lifecycle and rebuild renderer/physics state on map-loaded callbacks.
- Initial spawn generation requests the required chunk and uses failure-aware reads.
- No lifecycle defect was confirmed in the prior audit.

## Known limitations and open risks (not all are confirmed bugs)

1. End-to-end runtime playability and seamless streaming still need verification.
2. Latest oversized-draw batching commit needs a backend-by-backend correctness review.
3. Fancy lighting is not supported in infinite worlds; Classic is forced.
4. Fixed render window is 21×21 horizontal chunks; increasing finite view-distance settings does not expand it.
5. Dense heavily edited chunks can exceed the soft resident-memory target.
6. Save is not atomic; interrupted writes can leave a partial destination file.
7. Physics queue saturation and inactive-area scheduling can delay physics updates.
8. 32-bit float world positions lose precision at extreme distances.
9. ICW1 save payload layout is build-dependent; ICW2 is the portable format.
10. Finite-world/.cw regression checks must be completed after the playable milestone.

## Working rules

- Work directly on the existing `master` branch. Never create a branch or force-push.
- Before each commit, fetch the latest master ref and its commit/tree fresh; build on that exact tree and use an expected SHA when updating master.
- Re-fetch source after detecting a newer HEAD. Never reuse stale file blobs or tree SHAs.
- Keep changes focused; preserve existing line endings; abort if exact source anchors do not match.
- Do not edit README unless explicitly requested.
- Record confirmed defects separately from speculative risks. For every fix, record the commit SHA and the affected subsystem.
- Do not claim runtime/build/test verification unless it actually happened.

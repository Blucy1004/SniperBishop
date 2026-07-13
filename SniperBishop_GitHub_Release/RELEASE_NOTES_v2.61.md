# SniperBishop 2.61 Hybrid Tuned — alpha

First public hybrid FIRST_NET release.

## Changes

- Default 65% classical / 35% FIRST_NET evaluation
- Added `HybridGuard`
- Reduced NNUE trust on large evaluator disagreement and in late phases
- Widened static-evaluation pruning safety margins when NNUE participates
- Reduced accumulator and undo-snapshot storage from int64 to int32
- Fixed `NNUEBlend` UCI declaration
- Updated default network filename to `firstnet_v3.snnue`

## Files required at runtime

```text
SniperBishop.exe
firstnet_v3.snnue
```

Keep both files in the same directory.

## Verification

```text
perft(4) = 197281
network format = SBNNUE2
default NNUE blend = 35
HybridGuard = true
```

This is an experimental alpha release. Rating-list inclusion and playing strength are not yet established.

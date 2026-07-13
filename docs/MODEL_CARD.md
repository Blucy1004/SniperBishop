# FIRST_NET v3 model card

## Summary

FIRST_NET v3 is the neural component bundled with SniperBishop 2.61 Hybrid Tuned.

- Dataset size: 1,000,000 rows
- Construction: 500,000 original positions plus exact color/rank-mirror augmentation
- Source games: rated Lichess standard games
- Filtering: stronger-player games, non-bullet time controls, bots and time forfeits excluded
- Teacher: Stockfish node-limited evaluation
- Training: v2 warm start, 10 float epochs plus 3 quantization-aware-training epochs
- Runtime format: `SBNNUE2` / `.snnue`
- Hidden width: 256

## Intended use

The model is intended as a positional correction inside SniperBishop's hybrid evaluator. Pure 100% NNUE mode is retained for research and regression testing, but is not the recommended default release setting.

## Known limitations

- Offline evaluation error remains considerably larger in middlegame and endgame positions than in openings.
- Pure NNUE testing against the previous classical engine scored 0/10 in an early small match.
- The hybrid guard therefore reduces NNUE weight on large evaluator disagreement and in late phases.
- The model is not claimed to reproduce Stockfish evaluation exactly.

## Reproducibility

The network checksum is included in the repository root `CHECKSUMS.txt`.

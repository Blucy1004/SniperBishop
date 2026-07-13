# Benchmarks and experiment log

## Correctness smoke test

```text
perft(4) from the standard starting position: 197281
FIRST_NET incremental vs scratch oracle at start position: 0 cp difference
```

## Historical local matches

These are development checks, not official ratings.

### Classical predecessor vs Piccolo baseline

A prior classical-only configuration recorded:

```text
20 games: 6 wins, 2 losses, 12 draws
score: 60%
```

### FIRST_NET 10k/100k-era pure network test

```text
9 games: 0 wins, 9 losses, 0 draws
```

### FIRST_NET v3 one-million-position pure network test

```text
10 completed games: 0 wins, 10 losses, 0 draws
colors: 0/5 as White, 0/5 as Black
time forfeits: 0
illegal/desync markers: 0
```

The match was manually stopped after 10 completed games. This result motivated the hybrid main line.

## Release-testing recommendation

Before calling a version stable:

1. Run paired matches with fixed seed and switched colors.
2. Compare classical, hybrid 20/35/50 and pure NNUE configurations.
3. Use at least 100 games for preliminary tuning and substantially more for small Elo differences.
4. Record engine, opponent and network SHA-256 values.

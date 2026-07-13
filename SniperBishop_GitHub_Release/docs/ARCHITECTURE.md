# Architecture overview

SniperBishop is currently maintained as a single-file C++17 engine while the project stabilizes.

## Board and move generation

- Bitboards for piece sets and occupancy
- Magic-bitboard sliding attacks
- Pseudo-legal generation followed by legal make/unmake validation
- Castling, en passant and all promotion types
- Zobrist position keys and repetition history

## Search

- Iterative deepening
- Principal variation search
- Aspiration windows
- Transposition table
- Quiescence search
- Null-move pruning
- Late-move reductions
- Reverse futility and futility pruning
- Killer and history heuristics

## Evaluation

`evaluate()` can operate in three modes:

1. Classical only (`NNUEBlend=0`)
2. Hybrid (`NNUEBlend` between 1 and 99)
3. Pure FIRST_NET (`NNUEBlend=100`)

In hybrid mode, `HybridGuard` reduces the effective neural weight when:

- absolute disagreement exceeds 180, 300 or 500 centipawns;
- the phase context indicates a late middlegame or endgame.

Search pruning margins are widened when NNUE participates, reflecting greater static-evaluation uncertainty.

## Incremental FIRST_NET

Each position stores White- and Black-perspective accumulators. Normal piece moves update both incrementally. King-bucket changes invalidate the relevant perspective and trigger a rebuild when required.

# SniperBishop

**SniperBishop 2.61 Hybrid Tuned** is a Windows-first C++17 UCI chess engine by **Blucy1004**.

The current development release combines a handcrafted classical evaluator with FIRST_NET v3, a 256-hidden-unit efficiently updatable neural evaluator trained from a one-million-position dataset.

**SniperBishop 은신비숍** 은  **Blucy1004** 가 만든 윈도우 C++17 UCI 체스 엔진입니다.
한국산 엔진이며 현재 지속적인 업데이트가 진행 중입니다.

> Status: experimental alpha. The hybrid evaluator is the current main line; pure NNUE mode remains available for testing.

## Highlights

- Magic bitboards and legal move generation
- Zobrist hashing and transposition table
- Iterative deepening, aspiration windows and PVS
- Quiescence search, null-move pruning, LMR, killer/history ordering
- Incrementally updated FIRST_NET accumulators
- Classical/NNUE hybrid evaluation with disagreement and endgame guards
- UCI protocol and terminal play mode
- Built-in FEN, PGN analysis and perft commands
- Local CuteChess match runner: **Horizontal Rook**
- Post-match report tool: **Backing Knight**

## Current evaluation defaults

- Classical evaluation: 65%
- FIRST_NET v3: 35%
- `HybridGuard`: enabled
- The NNUE weight is reduced automatically when the evaluators strongly disagree or the position approaches an endgame.

## Build

### Visual Studio / MSVC

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output:

```text
build/Release/SniperBishop.exe
build/Release/firstnet_v3.snnue
```

### GCC / Clang

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Quick UCI check

Run the executable and enter:

```text
uci
isready
position startpos
eval
go depth 10
quit
```

Expected network messages include `NNUE ready` or `NNUE loaded`.

## Main UCI options

| Option | Default | Meaning |
|---|---:|---|
| `Hash` | 64 | Transposition-table size in MB |
| `UseNNUE` | true | Enables FIRST_NET |
| `EvalFile` | `firstnet_v3.snnue` | Network file placed beside the executable |
| `NNUEBlend` | 35 | Neural share from 0 to 100 |
| `HybridGuard` | true | Reduces neural trust on large disagreement and in late phases |
| `Underpromotion` | true | Enables underpromotion search |

Pure classical:

```text
setoption name NNUEBlend value 0
```

Pure FIRST_NET:

```text
setoption name NNUEBlend value 100
```

Recommended development mode:

```text
setoption name NNUEBlend value 35
setoption name HybridGuard value true
```

## Verification

The current source and network were smoke-tested with:

```text
perft(4) = 197281
FIRST_NET incremental/scratch difference = 0 cp at start position
```

SHA-256 checksums are listed in `CHECKSUMS.txt`.

## Repository layout

```text
src/                     Engine source
networks/                FIRST_NET v3 network
tools/horizontal_rook.py CuteChess match runner
tools/backing_knight.py  Post-match report generator
docs/                    Architecture, model and testing notes
scripts/                 Local build and verification helpers
```

## Rating-list testing

See [`docs/CCRL_SUBMISSION.md`](docs/CCRL_SUBMISSION.md) for the release checklist and a ready-to-send testing request.

## License

Source code and the included FIRST_NET v3 network are released under the MIT License unless a file states otherwise.

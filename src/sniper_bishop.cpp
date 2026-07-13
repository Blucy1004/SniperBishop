// ============================================================
//  Sniper Bishop Chess Engine 2.61 FIRST_NET Hybrid Tuned DEV
//  - Magic bitboards, Zobrist hashing
//  - Alpha-beta (PVS) + transposition table
//  - Null-move pruning, LMR, killer/history ordering
//  - Quiescence search, iterative deepening, aspiration windows
//  - UCI protocol + interactive CLI play mode + built-in paste/file PGN analyzer + small opening book
//  UI features:
//  - Colored board (Win10+ ANSI, auto fallback to plain ASCII)
//  - Board flips to your perspective, last move highlighted [ ]
//  - Captured pieces / material balance display
//  - Commands: history, pgn, fen, new, flip, level, hint, eval, undo
//  All output is plain ASCII English (no encoding issues on Windows).
//  Build (GCC/Clang):  g++ -O2 -std=c++17 -o sniper_bishop sniper_bishop.cpp
//  Build (MSVC x64):   cl /O2 /EHsc /std:c++17 sniper_bishop.cpp
// ============================================================
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#include <intrin.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <map>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <array>
#include <filesystem>
#include <limits>
using namespace std;
typedef uint64_t U64;

// ---------------- basics ----------------
enum { WHITE, BLACK };
enum { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE };
const int INF  = 32000;
const int MATE = 31000;
const U64 FILE_A = 0x0101010101010101ULL;
const U64 FILE_H = FILE_A << 7;
const U64 RANK_1 = 0xFFULL;

#ifdef _MSC_VER
static inline int lsb(U64 b)      { unsigned long i; _BitScanForward64(&i, b); return (int)i; }
static inline int popcnt(U64 b)   { return (int)__popcnt64(b); }
#else
static inline int lsb(U64 b)      { return __builtin_ctzll(b); }
static inline int popcnt(U64 b)   { return __builtin_popcountll(b); }
#endif
static inline int poplsb(U64 &b)  { int s = lsb(b); b &= b - 1; return s; }
static inline U64 bit(int sq)     { return 1ULL << sq; }

static long long nowMs() {
    return chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------- console UI ----------------
// ANSI colors work on Windows 10+ once virtual terminal processing is
// enabled. If that fails (old console), we silently fall back to plain
// ASCII so nothing ever renders as garbage.
static bool useColor = false;
static void initConsole() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
        if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            useColor = true;
#else
    useColor = true;
#endif
}
static const char *CLR_W     = "\x1b[1;97m";  // white pieces: bright white
static const char *CLR_B     = "\x1b[1;96m";  // black pieces: bright cyan
static const char *CLR_DIM   = "\x1b[90m";    // empty squares
static const char *CLR_HL    = "\x1b[1;93m";  // last-move brackets
static const char *CLR_RESET = "\x1b[0m";

// ---------------- attack tables ----------------
U64 pawnAtt[2][64], knightAtt[64], kingAtt[64];
U64 bMask[64], rMask[64], bMagic[64], rMagic[64];
int bShift[64], rShift[64];
static U64 bTable[64][512];
static U64 rTable[64][4096];

static U64 rngState = 0x9E3779B97F4A7C15ULL;
static U64 rand64() {
    rngState ^= rngState >> 12; rngState ^= rngState << 25; rngState ^= rngState >> 27;
    return rngState * 0x2545F4914F6CDD1DULL;
}
static U64 sparseRand() { return rand64() & rand64() & rand64(); }

static U64 slideAtt(int sq, U64 block, bool rook) {
    static const int RD[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    static const int BD[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
    U64 att = 0; int r0 = sq / 8, f0 = sq % 8;
    for (int d = 0; d < 4; d++) {
        int dr = rook ? RD[d][0] : BD[d][0];
        int df = rook ? RD[d][1] : BD[d][1];
        for (int r = r0 + dr, f = f0 + df; r >= 0 && r < 8 && f >= 0 && f < 8; r += dr, f += df) {
            att |= bit(r * 8 + f);
            if (block & bit(r * 8 + f)) break;
        }
    }
    return att;
}
static U64 slideMask(int sq, bool rook) {
    U64 m = 0; int r0 = sq / 8, f0 = sq % 8;
    if (rook) {
        for (int r = r0 + 1; r < 7; r++) m |= bit(r * 8 + f0);
        for (int r = r0 - 1; r > 0; r--) m |= bit(r * 8 + f0);
        for (int f = f0 + 1; f < 7; f++) m |= bit(r0 * 8 + f);
        for (int f = f0 - 1; f > 0; f--) m |= bit(r0 * 8 + f);
    } else {
        static const int BD[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int d = 0; d < 4; d++)
            for (int r = r0 + BD[d][0], f = f0 + BD[d][1];
                 r > 0 && r < 7 && f > 0 && f < 7; r += BD[d][0], f += BD[d][1])
                m |= bit(r * 8 + f);
    }
    return m;
}
static U64 setOcc(int index, int bits, U64 mask) {
    U64 occ = 0;
    for (int i = 0; i < bits; i++) {
        int sq = poplsb(mask);
        if (index & (1 << i)) occ |= bit(sq);
    }
    return occ;
}
static void initMagics(bool rook) {
    for (int sq = 0; sq < 64; sq++) {
        U64 mask = slideMask(sq, rook);
        int bits = popcnt(mask);
        int n = 1 << bits;
        U64 occs[4096], atts[4096];
        for (int i = 0; i < n; i++) {
            occs[i] = setOcc(i, bits, mask);
            atts[i] = slideAtt(sq, occs[i], rook);
        }
        U64 magic = 0;
        for (;;) {
            magic = sparseRand();
            if (popcnt((mask * magic) >> 56) < 6) continue;
            U64 used[4096];
            memset(used, 0, sizeof(U64) * n);
            bool fail = false;
            for (int i = 0; i < n && !fail; i++) {
                int idx = (int)((occs[i] * magic) >> (64 - bits));
                if (!used[idx]) used[idx] = atts[i];
                else if (used[idx] != atts[i]) fail = true;
            }
            if (!fail) break;
        }
        if (rook) {
            rMask[sq] = mask; rMagic[sq] = magic; rShift[sq] = 64 - bits;
            memset(rTable[sq], 0, sizeof(rTable[sq]));
            for (int i = 0; i < n; i++)
                rTable[sq][(occs[i] * magic) >> (64 - bits)] = atts[i];
        } else {
            bMask[sq] = mask; bMagic[sq] = magic; bShift[sq] = 64 - bits;
            memset(bTable[sq], 0, sizeof(bTable[sq]));
            for (int i = 0; i < n; i++)
                bTable[sq][(occs[i] * magic) >> (64 - bits)] = atts[i];
        }
    }
}
static inline U64 bishopAtt(int sq, U64 occ) {
    return bTable[sq][((occ & bMask[sq]) * bMagic[sq]) >> bShift[sq]];
}
static inline U64 rookAtt(int sq, U64 occ) {
    return rTable[sq][((occ & rMask[sq]) * rMagic[sq]) >> rShift[sq]];
}
static inline U64 queenAtt(int sq, U64 occ) { return bishopAtt(sq, occ) | rookAtt(sq, occ); }

static void initLeapers() {
    static const int NK[8][2] = {{2,1},{2,-1},{-2,1},{-2,-1},{1,2},{1,-2},{-1,2},{-1,-2}};
    static const int KK[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int sq = 0; sq < 64; sq++) {
        int r = sq / 8, f = sq % 8;
        U64 n = 0, k = 0;
        for (int d = 0; d < 8; d++) {
            int nr = r + NK[d][0], nf = f + NK[d][1];
            if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) n |= bit(nr * 8 + nf);
            nr = r + KK[d][0]; nf = f + KK[d][1];
            if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) k |= bit(nr * 8 + nf);
        }
        knightAtt[sq] = n; kingAtt[sq] = k;
        U64 b = bit(sq);
        pawnAtt[WHITE][sq] = ((b << 7) & ~FILE_H) | ((b << 9) & ~FILE_A);
        pawnAtt[BLACK][sq] = ((b >> 7) & ~FILE_A) | ((b >> 9) & ~FILE_H);
    }
}

// eval helper masks
U64 fileMask[8], adjFileMask[8], passedMask[2][64];
static void initEvalMasks() {
    for (int f = 0; f < 8; f++) {
        fileMask[f] = FILE_A << f;
        adjFileMask[f] = 0;
        if (f > 0) adjFileMask[f] |= FILE_A << (f - 1);
        if (f < 7) adjFileMask[f] |= FILE_A << (f + 1);
    }
    for (int sq = 0; sq < 64; sq++) {
        int r = sq / 8, f = sq % 8;
        U64 files = fileMask[f] | adjFileMask[f];
        U64 wf = 0, bf = 0;
        for (int rr = r + 1; rr < 8; rr++) wf |= (files & (RANK_1 << (rr * 8)));
        for (int rr = r - 1; rr >= 0; rr--) bf |= (files & (RANK_1 << (rr * 8)));
        passedMask[WHITE][sq] = wf;
        passedMask[BLACK][sq] = bf;
    }
}

// ---------------- zobrist ----------------
U64 zPiece[2][6][64], zSide, zCastle[16], zEp[8];
static void initZobrist() {
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++)
            for (int s = 0; s < 64; s++) zPiece[c][p][s] = rand64();
    zSide = rand64();
    for (int i = 0; i < 16; i++) zCastle[i] = rand64();
    for (int i = 0; i < 8; i++) zEp[i] = rand64();
}

// ---------------- position ----------------
static constexpr int FIRSTNET_HIDDEN = 256;

struct Position {
    U64 pieces[2][6];
    U64 occ[2], occAll;
    int board[64];      // piece type or NO_PIECE
    int side;
    int castle;         // 1=WK 2=WQ 4=BK 8=BQ
    int ep;             // en-passant square or -1
    int halfmove;
    U64 key;

    // FIRST_NET pre-activation feature-transformer sums.
    // Perspective 0 = White king view, perspective 1 = Black king view.
    // Context (STM/phase/castling rights) is deliberately not stored here.
    mutable array<int32_t, FIRSTNET_HIDDEN> nnueAcc[2];
    mutable uint8_t nnueValidMask;      // bit 0=White, bit 1=Black
    mutable uint64_t nnueGeneration;    // invalidates states after network reload
};

namespace FirstNet {
    static void onPieceMove(Position &pos, int color, int piece, int from, int to);
    static void onPieceAdd(Position &pos, int color, int piece, int square);
    static void onPieceRemove(Position &pos, int color, int piece, int square);
    static void finishLegalMove(Position &pos, int movingColor, int movedPiece);
    static void invalidate(Position &pos);
    static bool trackingActive(const Position &pos);
    static void noteAccumulatorCopy();
}
static bool firstNetMutationSuppressed = false;

int castleMaskTbl[64];
static void initCastleMask() {
    for (int i = 0; i < 64; i++) castleMaskTbl[i] = 15;
    castleMaskTbl[0]  = 13; castleMaskTbl[4]  = 12; castleMaskTbl[7]  = 14;
    castleMaskTbl[56] = 7;  castleMaskTbl[60] = 3;  castleMaskTbl[63] = 11;
}

static U64 computeKey(const Position &pos) {
    U64 k = 0;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++) {
            U64 b = pos.pieces[c][p];
            while (b) k ^= zPiece[c][p][poplsb(b)];
        }
    if (pos.side == BLACK) k ^= zSide;
    k ^= zCastle[pos.castle];
    if (pos.ep >= 0) k ^= zEp[pos.ep % 8];
    return k;
}

static int colorAt(const Position &pos, int sq) {
    if (pos.occ[WHITE] & bit(sq)) return WHITE;
    if (pos.occ[BLACK] & bit(sq)) return BLACK;
    return -1;
}

static bool sqAttacked(const Position &pos, int sq, int by) {
    if (pawnAtt[by ^ 1][sq] & pos.pieces[by][PAWN]) return true;
    if (knightAtt[sq] & pos.pieces[by][KNIGHT]) return true;
    if (kingAtt[sq] & pos.pieces[by][KING]) return true;
    if (bishopAtt(sq, pos.occAll) & (pos.pieces[by][BISHOP] | pos.pieces[by][QUEEN])) return true;
    if (rookAtt(sq, pos.occAll)   & (pos.pieces[by][ROOK]   | pos.pieces[by][QUEEN])) return true;
    return false;
}
static bool inCheck(const Position &pos) {
    return sqAttacked(pos, lsb(pos.pieces[pos.side][KING]), pos.side ^ 1);
}

// ---------------- moves ----------------
// move = from | to<<6 | promo<<12  (promo: 0 or KNIGHT..QUEEN)
static inline int mvFrom(int m)  { return m & 63; }
static inline int mvTo(int m)    { return (m >> 6) & 63; }
static inline int mvPromo(int m) { return (m >> 12) & 7; }
static inline int makeMv(int f, int t, int p = 0) { return f | (t << 6) | (p << 12); }

struct MoveList {
    int moves[256]; int score[256]; int cnt = 0;
    void add(int m) { moves[cnt++] = m; }
};

static void genMoves(const Position &pos, MoveList &ml, bool capsOnly) {
    int us = pos.side, them = us ^ 1;
    U64 own = pos.occ[us], enemy = pos.occ[them], all = pos.occAll;
    // A legal chess move never captures the opponent king. If pseudo-move
    // generation includes king-captures, search can make a move that removes
    // the king from the board; later inCheck()/genMoves() then call lsb(0),
    // producing undefined behavior and nonsense PV/bestmove strings. This was
    // the real root behind many "rook moves like bishop" / ghost move cases.
    U64 enemyKing = pos.pieces[them][KING];
    U64 enemyNoKing = enemy & ~enemyKing;
    U64 target = capsOnly ? enemyNoKing : (~own & ~enemyKing);

    // pawns
    U64 pawns = pos.pieces[us][PAWN];
    int push = us == WHITE ? 8 : -8;
    int promRank = us == WHITE ? 7 : 0;
    int dblRank  = us == WHITE ? 1 : 6;
    U64 b = pawns;
    while (b) {
        int from = poplsb(b);
        int r = from / 8;
        // captures & ep
        U64 caps = pawnAtt[us][from] & enemyNoKing;
        while (caps) {
            int to = poplsb(caps);
            if (to / 8 == promRank)
                for (int p = QUEEN; p >= KNIGHT; p--) ml.add(makeMv(from, to, p));
            else ml.add(makeMv(from, to));
        }
        if (pos.ep >= 0 && (pawnAtt[us][from] & bit(pos.ep)))
            ml.add(makeMv(from, pos.ep));
        // pushes
        int to1 = from + push;
        if (to1 >= 0 && to1 < 64 && !(all & bit(to1))) {
            if (to1 / 8 == promRank) {
                for (int p = QUEEN; p >= KNIGHT; p--) ml.add(makeMv(from, to1, p));
            } else if (!capsOnly) {
                ml.add(makeMv(from, to1));
                int to2 = from + 2 * push;
                if (r == dblRank && to2 >= 0 && to2 < 64 && !(all & bit(to2)))
                    ml.add(makeMv(from, to2));
            }
        }
    }
    // knights
    b = pos.pieces[us][KNIGHT];
    while (b) { int f = poplsb(b); U64 a = knightAtt[f] & target;
        while (a) ml.add(makeMv(f, poplsb(a))); }
    // bishops
    b = pos.pieces[us][BISHOP];
    while (b) { int f = poplsb(b); U64 a = bishopAtt(f, all) & target;
        while (a) ml.add(makeMv(f, poplsb(a))); }
    // rooks
    b = pos.pieces[us][ROOK];
    while (b) { int f = poplsb(b); U64 a = rookAtt(f, all) & target;
        while (a) ml.add(makeMv(f, poplsb(a))); }
    // queens
    b = pos.pieces[us][QUEEN];
    while (b) { int f = poplsb(b); U64 a = queenAtt(f, all) & target;
        while (a) ml.add(makeMv(f, poplsb(a))); }
    // king
    {
        int f = lsb(pos.pieces[us][KING]);
        U64 a = kingAtt[f] & target;
        while (a) ml.add(makeMv(f, poplsb(a)));
        if (!capsOnly) {
            if (us == WHITE) {
                if ((pos.castle & 1) && (pos.pieces[WHITE][ROOK] & bit(7)) && !(all & 0x60ULL) &&
                    !sqAttacked(pos, 4, BLACK) && !sqAttacked(pos, 5, BLACK) && !sqAttacked(pos, 6, BLACK))
                    ml.add(makeMv(4, 6));
                if ((pos.castle & 2) && (pos.pieces[WHITE][ROOK] & bit(0)) && !(all & 0x0EULL) &&
                    !sqAttacked(pos, 4, BLACK) && !sqAttacked(pos, 3, BLACK) && !sqAttacked(pos, 2, BLACK))
                    ml.add(makeMv(4, 2));
            } else {
                if ((pos.castle & 4) && (pos.pieces[BLACK][ROOK] & bit(63)) && !(all & (0x60ULL << 56)) &&
                    !sqAttacked(pos, 60, WHITE) && !sqAttacked(pos, 61, WHITE) && !sqAttacked(pos, 62, WHITE))
                    ml.add(makeMv(60, 62));
                if ((pos.castle & 8) && (pos.pieces[BLACK][ROOK] & bit(56)) && !(all & (0x0EULL << 56)) &&
                    !sqAttacked(pos, 60, WHITE) && !sqAttacked(pos, 59, WHITE) && !sqAttacked(pos, 58, WHITE))
                    ml.add(makeMv(60, 58));
            }
        }
    }
}

// ---------------- make / unmake ----------------
struct Undo {
    int move, cap, capSq, castle, ep, halfmove;
    U64 key;
    array<int32_t, FIRSTNET_HIDDEN> nnueAcc[2];
    uint8_t nnueValidMask;
    uint64_t nnueGeneration;
    bool nnueSnapshot;
};
static Undo undoStack[2048];
static int  undoSp = 0;
static U64  repKeys[2048];
static int  repIdx = 0;

static inline bool sqOk(int sq) { return sq >= 0 && sq < 64; }
static inline bool pieceOk(int p) { return p >= PAWN && p <= KING; }
static inline bool promoOk(int p) { return p == KNIGHT || p == BISHOP || p == ROOK || p == QUEEN; }
static inline bool hasPiece(const Position &pos, int c, int p, int sq) {
    return c >= WHITE && c <= BLACK && pieceOk(p) && sqOk(sq) && (pos.pieces[c][p] & bit(sq));
}

static inline void movePiece(Position &pos, int c, int p, int from, int to) {
    if (!firstNetMutationSuppressed)
        FirstNet::onPieceMove(pos, c, p, from, to);
    U64 bf = bit(from), bt = bit(to);
    pos.pieces[c][p] &= ~bf;
    pos.pieces[c][p] |=  bt;
    pos.occ[c]       &= ~bf;
    pos.occ[c]       |=  bt;
    pos.board[from] = NO_PIECE; pos.board[to] = p;
    pos.key ^= zPiece[c][p][from] ^ zPiece[c][p][to];
}
static inline void addPiece(Position &pos, int c, int p, int sq) {
    U64 b = bit(sq);
    if (!(pos.pieces[c][p] & b)) {
        if (!firstNetMutationSuppressed)
            FirstNet::onPieceAdd(pos, c, p, sq);
        pos.pieces[c][p] |= b; pos.occ[c] |= b;
        pos.key ^= zPiece[c][p][sq];
    }
    pos.board[sq] = p;
}
static inline void removePiece(Position &pos, int c, int p, int sq) {
    U64 b = bit(sq);
    if (pos.pieces[c][p] & b) {
        if (!firstNetMutationSuppressed)
            FirstNet::onPieceRemove(pos, c, p, sq);
        pos.pieces[c][p] &= ~b; pos.occ[c] &= ~b;
        pos.key ^= zPiece[c][p][sq];
    }
    pos.board[sq] = NO_PIECE;
}

static bool makeMove(Position &pos, int m) {
    int us = pos.side, them = us ^ 1;
    int from = mvFrom(m), to = mvTo(m), promo = mvPromo(m);
    int piece = sqOk(from) ? pos.board[from] : NO_PIECE;

    // Hard legality/sanity gate. Search-generated moves should already be
    // pseudo-legal, but TT/PV corruption, bad GUI input, stale castling
    // rights in a FEN, or a promotion-string desync must never be able to
    // create ghost pieces by moving from an empty square or by castling a
    // rook that is not there.
    if (!sqOk(from) || !sqOk(to) || !pieceOk(piece)) return false;
    if (!hasPiece(pos, us, piece, from)) return false;
    int toColor = colorAt(pos, to);
    if (toColor == us) return false;
    // Absolutely forbid king captures. Checkmate is represented by the side
    // to move having no legal replies while in check, not by physically
    // capturing the king. Allowing this corrupts the position (missing king)
    // and makes later lsb(kingBitboard) undefined.
    if (toColor == them && pos.board[to] == KING) return false;
    if (promo && (piece != PAWN || !promoOk(promo))) return false;
    if (piece != PAWN && promo) return false;

    // Pseudo-legal shape validation. This keeps corrupted/stale move ints
    // from being able to move a rook like a queen, a pawn like a rook, etc.
    bool pseudo = false;
    if (piece == PAWN) {
        int dir = us == WHITE ? 8 : -8;
        int startRank = us == WHITE ? 1 : 6;
        int promRank = us == WHITE ? 7 : 0;
        int fromRank = from / 8;
        bool reachesPromotion = (to / 8 == promRank);
        if (reachesPromotion != (promo != 0)) return false;
        if (to == from + dir && toColor < 0) {
            pseudo = true;
        } else if (fromRank == startRank && to == from + 2 * dir && toColor < 0 &&
                   !(pos.occAll & bit(from + dir))) {
            pseudo = true;
        } else if ((pawnAtt[us][from] & bit(to)) && toColor == them) {
            pseudo = true;
        } else if ((pawnAtt[us][from] & bit(to)) && toColor < 0) {
            int capSq = us == WHITE ? to - 8 : to + 8;
            pseudo = (to == pos.ep && sqOk(capSq) && hasPiece(pos, them, PAWN, capSq));
        }
    } else if (piece == KNIGHT) {
        pseudo = knightAtt[from] & bit(to);
    } else if (piece == BISHOP) {
        pseudo = bishopAtt(from, pos.occAll) & bit(to);
    } else if (piece == ROOK) {
        pseudo = rookAtt(from, pos.occAll) & bit(to);
    } else if (piece == QUEEN) {
        pseudo = queenAtt(from, pos.occAll) & bit(to);
    } else if (piece == KING) {
        if (abs(to - from) == 2) {
            int rookFrom = (to > from) ? to + 1 : to - 2;
            int rookTo   = (to > from) ? to - 1 : to + 1;
            int castleBit = us == WHITE ? (to > from ? 1 : 2) : (to > from ? 4 : 8);
            pseudo = (pos.castle & castleBit) && sqOk(rookFrom) && sqOk(rookTo) &&
                     hasPiece(pos, us, ROOK, rookFrom) &&
                     !(pos.occAll & bit(to)) && !(pos.occAll & bit(rookTo)) &&
                     !sqAttacked(pos, from, them) && !sqAttacked(pos, rookTo, them) &&
                     !sqAttacked(pos, to, them);
        } else {
            pseudo = kingAtt[from] & bit(to);
        }
    }
    if (!pseudo) return false;
    if (undoSp >= 2047 || repIdx >= 2047) return false;

    Undo &u = undoStack[undoSp++];
    u.move = m; u.castle = pos.castle; u.ep = pos.ep;
    u.halfmove = pos.halfmove; u.key = pos.key;
    u.cap = NO_PIECE; u.capSq = -1;
    u.nnueSnapshot = FirstNet::trackingActive(pos);
    if (u.nnueSnapshot) {
        u.nnueAcc[WHITE] = pos.nnueAcc[WHITE];
        u.nnueAcc[BLACK] = pos.nnueAcc[BLACK];
        u.nnueValidMask = pos.nnueValidMask;
        u.nnueGeneration = pos.nnueGeneration;
        FirstNet::noteAccumulatorCopy();
    }
    repKeys[repIdx++] = pos.key;

    pos.key ^= zCastle[pos.castle];
    if (pos.ep >= 0) pos.key ^= zEp[pos.ep % 8];

    pos.halfmove++;
    int newEp = -1;

    // capture (incl. en passant)
    if (pos.board[to] != NO_PIECE) {
        u.cap = pos.board[to]; u.capSq = to;
        removePiece(pos, them, u.cap, to);
        pos.halfmove = 0;
    } else if (piece == PAWN && (to & 7) != (from & 7)) {
        int capSq = us == WHITE ? to - 8 : to + 8;
        u.cap = PAWN; u.capSq = capSq;
        removePiece(pos, them, PAWN, capSq);
        pos.halfmove = 0;
    }

    movePiece(pos, us, piece, from, to);

    if (piece == PAWN) {
        pos.halfmove = 0;
        if (promo) {
            removePiece(pos, us, PAWN, to);
            addPiece(pos, us, promo, to);
        } else if (abs(to - from) == 16) {
            newEp = (from + to) / 2;
        }
    } else if (piece == KING) {
        if (to - from == 2)      movePiece(pos, us, ROOK, to + 1, to - 1);
        else if (from - to == 2) movePiece(pos, us, ROOK, to - 2, to + 1);
    }

    pos.castle &= castleMaskTbl[from] & castleMaskTbl[to];
    pos.ep = newEp;
    pos.key ^= zCastle[pos.castle];
    if (pos.ep >= 0) pos.key ^= zEp[pos.ep % 8];
    pos.side = them;
    pos.key ^= zSide;
    pos.occAll = pos.occ[WHITE] | pos.occ[BLACK];

    // legality
    if (sqAttacked(pos, lsb(pos.pieces[us][KING]), them)) {
        // undo
        pos.side = us; pos.key = u.key; pos.castle = u.castle;
        pos.ep = u.ep; pos.halfmove = u.halfmove;
        // reverse piece moves without touching NNUE; restore the exact snapshot below
        U64 savedKey = pos.key;
        firstNetMutationSuppressed = true;
        int pcAtTo = pos.board[to];
        movePiece(pos, us, pcAtTo, to, from);
        if (promo) { removePiece(pos, us, promo, from); addPiece(pos, us, PAWN, from); }
        if (piece == KING) {
            if (to - from == 2)      movePiece(pos, us, ROOK, to - 1, to + 1);
            else if (from - to == 2) movePiece(pos, us, ROOK, to + 1, to - 2);
        }
        if (u.cap != NO_PIECE) addPiece(pos, them, u.cap, u.capSq);
        firstNetMutationSuppressed = false;
        pos.key = savedKey;
        pos.occAll = pos.occ[WHITE] | pos.occ[BLACK];
        if (u.nnueSnapshot) {
            pos.nnueAcc[WHITE] = u.nnueAcc[WHITE];
            pos.nnueAcc[BLACK] = u.nnueAcc[BLACK];
            pos.nnueValidMask = u.nnueValidMask;
            pos.nnueGeneration = u.nnueGeneration;
        } else {
            FirstNet::invalidate(pos);
        }
        undoSp--; repIdx--;
        return false;
    }
    FirstNet::finishLegalMove(pos, us, piece);
    return true;
}

static void unmakeMove(Position &pos) {
    Undo &u = undoStack[--undoSp];
    repIdx--;
    int m = u.move;
    int from = mvFrom(m), to = mvTo(m), promo = mvPromo(m);
    int them = pos.side, us = them ^ 1;

    // Board reversal is followed by an exact accumulator snapshot restore.
    // Suppressing hooks avoids pointless reverse delta work on the hot path.
    firstNetMutationSuppressed = true;
    int pcAtTo = pos.board[to];
    movePiece(pos, us, pcAtTo, to, from);
    if (promo) { removePiece(pos, us, promo, from); addPiece(pos, us, PAWN, from); }
    if (pcAtTo == KING) {
        if (to - from == 2)      movePiece(pos, us, ROOK, to - 1, to + 1);
        else if (from - to == 2) movePiece(pos, us, ROOK, to + 1, to - 2);
    }
    if (u.cap != NO_PIECE) addPiece(pos, them, u.cap, u.capSq);
    firstNetMutationSuppressed = false;

    pos.side = us; pos.castle = u.castle; pos.ep = u.ep;
    pos.halfmove = u.halfmove; pos.key = u.key;
    pos.occAll = pos.occ[WHITE] | pos.occ[BLACK];
    if (u.nnueSnapshot) {
        pos.nnueAcc[WHITE] = u.nnueAcc[WHITE];
        pos.nnueAcc[BLACK] = u.nnueAcc[BLACK];
        pos.nnueValidMask = u.nnueValidMask;
        pos.nnueGeneration = u.nnueGeneration;
    } else {
        FirstNet::invalidate(pos);
    }
}

// null move
struct NullUndo { int ep; U64 key; };
static void makeNull(Position &pos, NullUndo &nu) {
    nu.ep = pos.ep; nu.key = pos.key;
    if (pos.ep >= 0) pos.key ^= zEp[pos.ep % 8];
    pos.ep = -1;
    pos.side ^= 1; pos.key ^= zSide;
    repKeys[repIdx++] = nu.key;
}
static void unmakeNull(Position &pos, const NullUndo &nu) {
    repIdx--;
    pos.side ^= 1; pos.ep = nu.ep; pos.key = nu.key;
}

// ---------------- FEN / board IO ----------------
static const char *START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static void clearPos(Position &pos) {
    memset(&pos, 0, sizeof(pos));
    for (int i = 0; i < 64; i++) pos.board[i] = NO_PIECE;
    pos.ep = -1;
}

static bool setFen(Position &pos, const string &fen) {
    clearPos(pos);
    istringstream ss(fen);
    string placement, stm, cast, eps;
    int half = 0, full = 1;
    ss >> placement >> stm >> cast >> eps;
    if (!(ss >> half)) half = 0;
    if (!(ss >> full)) full = 1;
    int r = 7, f = 0;
    for (char ch : placement) {
        if (ch == '/') { r--; f = 0; continue; }
        if (isdigit((unsigned char)ch)) { f += ch - '0'; continue; }
        int c = isupper((unsigned char)ch) ? WHITE : BLACK;
        int p;
        switch (tolower((unsigned char)ch)) {
            case 'p': p = PAWN; break;   case 'n': p = KNIGHT; break;
            case 'b': p = BISHOP; break; case 'r': p = ROOK; break;
            case 'q': p = QUEEN; break;  case 'k': p = KING; break;
            default: return false;
        }
        int sq = r * 8 + f;
        pos.pieces[c][p] |= bit(sq); pos.occ[c] |= bit(sq); pos.board[sq] = p;
        f++;
    }
    pos.side = (stm == "b") ? BLACK : WHITE;
    pos.castle = 0;
    if (cast.find('K') != string::npos) pos.castle |= 1;
    if (cast.find('Q') != string::npos) pos.castle |= 2;
    if (cast.find('k') != string::npos) pos.castle |= 4;
    if (cast.find('q') != string::npos) pos.castle |= 8;
    pos.ep = -1;
    if (eps.size() == 2) pos.ep = (eps[1] - '1') * 8 + (eps[0] - 'a');
    pos.halfmove = half;
    pos.occAll = pos.occ[WHITE] | pos.occ[BLACK];
    pos.key = computeKey(pos);
    undoSp = 0; repIdx = 0;
    return true;
}

static char promoChar(int promo) {
    switch (promo) {
        case KNIGHT: return 'n';
        case BISHOP: return 'b';
        case ROOK:   return 'r';
        case QUEEN:  return 'q';
        default:     return '\0';
    }
}
static string mvToStr(int m) {
    string s;
    s += 'a' + mvFrom(m) % 8; s += '1' + mvFrom(m) / 8;
    s += 'a' + mvTo(m) % 8;   s += '1' + mvTo(m) / 8;
    // Promotion output must be mapped by enum value explicitly. The old
    // table-driven version once printed queen promotions as rook promotions,
    // which made cutechess believe a promoted rook was moving like a queen in
    // the PV (e.g. a8=R followed by a8d5). Switch prevents that class forever.
    char pc = promoChar(mvPromo(m));
    if (pc) s += pc;
    return s;
}

// perspective: WHITE = white at bottom, BLACK = black at bottom.
// lastMove: highlight from/to squares with [ ] brackets (0 = none).
static void printBoard(const Position &pos, int perspective = WHITE, int lastMove = 0) {
    static const char pc[2][7] = {{'P','N','B','R','Q','K','.'},{'p','n','b','r','q','k','.'}};
    int lf = lastMove ? mvFrom(lastMove) : -1;
    int lt = lastMove ? mvTo(lastMove)   : -1;
    printf("\n    +---+---+---+---+---+---+---+---+\n");
    for (int i = 0; i < 8; i++) {
        int r = perspective == WHITE ? 7 - i : i;
        printf("  %d |", r + 1);
        for (int j = 0; j < 8; j++) {
            int f = perspective == WHITE ? j : 7 - j;
            int sq = r * 8 + f;
            char ch = '.';
            int c = colorAt(pos, sq);
            if (c >= 0) ch = pc[c][pos.board[sq]];
            bool hl = (sq == lf || sq == lt);
            char lb = hl ? '[' : ' ', rb = hl ? ']' : ' ';
            if (useColor) {
                const char *bcol = hl ? CLR_HL : "";
                const char *pcol = c == WHITE ? CLR_W : (c == BLACK ? CLR_B : CLR_DIM);
                printf("%s%c%s%s%c%s%s%c%s|",
                       bcol, lb, CLR_RESET, pcol, ch, CLR_RESET,
                       bcol, rb, CLR_RESET);
            } else {
                printf("%c%c%c|", lb, ch, rb);
            }
        }
        printf("\n    +---+---+---+---+---+---+---+---+\n");
    }
    if (perspective == WHITE) printf("      a   b   c   d   e   f   g   h\n");
    else                      printf("      h   g   f   e   d   c   b   a\n");
}

// ---------------- evaluation ----------------
static const int pieceVal[6] = {100, 320, 330, 500, 900, 0};
// tables in visual order (index 0 = a8); for white use sq^56
static const int pstPawn[64] = {
      0,  0,  0,  0,  0,  0,  0,  0,
     50, 50, 50, 50, 50, 50, 50, 50,
     10, 10, 20, 30, 30, 20, 10, 10,
      5,  5, 10, 25, 25, 10,  5,  5,
      0,  0,  0, 20, 20,  0,  0,  0,
      5, -5,-10,  0,  0,-10, -5,  5,
      5, 10, 10,-20,-20, 10, 10,  5,
      0,  0,  0,  0,  0,  0,  0,  0 };
static const int pstKnight[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50 };
static const int pstBishop[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5, 10, 10,  5,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -20,-10,-10,-10,-10,-10,-10,-20 };
static const int pstRook[64] = {
      0,  0,  0,  0,  0,  0,  0,  0,
      5, 10, 10, 10, 10, 10, 10,  5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
      0,  0,  0,  5,  5,  0,  0,  0 };
static const int pstQueen[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
      0,  0,  5,  5,  5,  5,  0, -5,
    -10,  5,  5,  5,  5,  5,  0,-10,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20 };
static const int pstKingMg[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20 };
static const int pstKingEg[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50,
    -30,-20,-10,  0,  0,-10,-20,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-30,  0,  0,  0,  0,-30,-30,
    -50,-30,-30,-30,-30,-30,-30,-50 };
static const int *pst[5] = {pstPawn, pstKnight, pstBishop, pstRook, pstQueen};
static const int passedBonus[8] = {0, 5, 10, 20, 35, 60, 100, 0};
static inline int kdist(int a, int b) {
    return max(abs(a / 8 - b / 8), abs(a % 8 - b % 8));
}

static int evaluateClassical(const Position &pos) {
    int mg = 0, eg = 0, phase = 0;
    static const int phaseW[6] = {0, 1, 1, 2, 4, 0};
    for (int c = 0; c < 2; c++) {
        int sign = c == WHITE ? 1 : -1;
        U64 own = pos.occ[c];
        // material + PST
        for (int p = PAWN; p <= QUEEN; p++) {
            U64 b = pos.pieces[c][p];
            phase += phaseW[p] * popcnt(b);
            while (b) {
                int sq = poplsb(b);
                int vi = c == WHITE ? sq ^ 56 : sq;
                int v = pieceVal[p] + pst[p][vi];
                mg += sign * v; eg += sign * v;
            }
        }
        // king PST
        {
            int ks = lsb(pos.pieces[c][KING]);
            int vi = c == WHITE ? ks ^ 56 : ks;
            mg += sign * pstKingMg[vi];
            eg += sign * pstKingEg[vi];
        }
        // bishop pair
        if (popcnt(pos.pieces[c][BISHOP]) >= 2) { mg += sign * 30; eg += sign * 40; }
        // pawn structure
        U64 myP = pos.pieces[c][PAWN], oppP = pos.pieces[c ^ 1][PAWN];
        // squares attacked by enemy pawns (for threats / outposts)
        U64 oppPawnAtt = c == WHITE
            ? (((oppP >> 7) & ~FILE_A) | ((oppP >> 9) & ~FILE_H))
            : (((oppP << 7) & ~FILE_H) | ((oppP << 9) & ~FILE_A));
        int myK = lsb(pos.pieces[c][KING]);
        int opK = lsb(pos.pieces[c ^ 1][KING]);
        U64 nonPawnPieces = (pos.occAll & ~pos.pieces[WHITE][PAWN] & ~pos.pieces[BLACK][PAWN]
                              & ~pos.pieces[WHITE][KING] & ~pos.pieces[BLACK][KING]);
        bool pawnRaceLike = popcnt(nonPawnPieces) <= 2;  // gate for the square rule below
        U64 b = myP;
        while (b) {
            int sq = poplsb(b);
            int f = sq % 8, r = sq / 8;
            int rr = c == WHITE ? r : 7 - r;
            if (popcnt(myP & fileMask[f]) > 1) { mg -= sign * 8; eg -= sign * 12; }
            if (!(myP & adjFileMask[f]))       { mg -= sign * 10; eg -= sign * 14; }
            if (!(passedMask[c][sq] & oppP)) {
                mg += sign * passedBonus[rr] / 2;
                eg += sign * passedBonus[rr];
                // protected passed pawn: another own pawn defends it
                if (pawnAtt[c ^ 1][sq] & myP) { mg += sign * 8; eg += sign * 18; }
                // connected passed pawn: an adjacent-file own pawn stands
                // right beside it, ready to advance together - these are
                // much harder to stop than lone runners
                if (myP & (adjFileMask[f] & (RANK_1 << (r * 8))))
                    { mg += sign * 10; eg += sign * 20; }
                // endgame: our king should escort the pawn, theirs should be far
                if (rr >= 3) {
                    int stopSq = c == WHITE ? sq + 8 : sq - 8;
                    eg += sign * (kdist(opK, stopSq) * 5 - kdist(myK, stopSq) * 3);
                }
                // square rule: in near-pure king+pawn races, if the enemy
                // king can't reach the queening square in time (and can't
                // get help from other pieces, since we gated on
                // pawnRaceLike), the pawn is essentially a guaranteed
                // promotion - a cheap, well-known rule search alone can
                // miss at shallow depth when the race is a bit long.
                if (pawnRaceLike && rr >= 2) {
                    int promoSq = c == WHITE ? (56 + f) : f;
                    int pawnStepsToPromo = 7 - rr;
                    int oppKingSteps = kdist(opK, promoSq);
                    int tempoEdge = (pos.side == c) ? 1 : 0;
                    if (oppKingSteps > pawnStepsToPromo + tempoEdge)
                        eg += sign * 90;
                }
            }
        }
        // rooks on open files + 7th rank + behind-passed-pawn + mobility
        b = pos.pieces[c][ROOK];
        while (b) {
            int sq = poplsb(b);
            int f = sq % 8, rr = c == WHITE ? sq / 8 : 7 - sq / 8;
            if (!(myP & fileMask[f])) {
                if (!(oppP & fileMask[f])) { mg += sign * 20; eg += sign * 10; }
                else                       { mg += sign * 10; eg += sign * 5; }
            }
            // rook behind a passed pawn on the same file (either side's -
            // behind our own runner it shepherds it forward; behind an
            // enemy runner it helps blockade/capture it from the rear)
            {
                U64 filePassers = fileMask[f] & (myP | oppP);
                U64 bb2 = filePassers;
                while (bb2) {
                    int psq = poplsb(bb2);
                    int pc = colorAt(pos, psq);
                    if (pc < 0) continue;
                    if (!(passedMask[pc][psq] & pos.pieces[pc ^ 1][PAWN])) {
                        bool rookBehind = pc == WHITE ? sq < psq : sq > psq;
                        if (rookBehind) { eg += sign * (pc == c ? 20 : 15); }
                    }
                }
            }
            // rook on the 7th rank pinning king or pawns to the back
            if (rr == 6) {
                int opkRR = c == WHITE ? opK / 8 : 7 - opK / 8;
                U64 seventh = c == WHITE ? (RANK_1 << 48) : (RANK_1 << 8);
                if (opkRR == 7 || (oppP & seventh)) { mg += sign * 20; eg += sign * 30; }
            }
            int mob = popcnt(rookAtt(sq, pos.occAll) & ~own);
            mg += sign * mob * 2; eg += sign * mob * 3;
        }
        // knights: mobility + outposts (advanced, pawn-protected, unassailable)
        b = pos.pieces[c][KNIGHT];
        while (b) {
            int sq = poplsb(b);
            int mob = popcnt(knightAtt[sq] & ~own);
            mg += sign * mob * 3; eg += sign * mob * 3;
            int f = sq % 8, rr = c == WHITE ? sq / 8 : 7 - sq / 8;
            if (rr >= 3 && rr <= 5 &&
                (pawnAtt[c ^ 1][sq] & myP) &&                       // guarded by own pawn
                !(passedMask[c][sq] & adjFileMask[f] & oppP)) {     // no pawn can chase it
                mg += sign * 22; eg += sign * 12;
            }
        }
        b = pos.pieces[c][BISHOP];
        while (b) { int mob = popcnt(bishopAtt(poplsb(b), pos.occAll) & ~own);
            mg += sign * mob * 3; eg += sign * mob * 3; }
        b = pos.pieces[c][QUEEN];
        while (b) { int mob = popcnt(queenAtt(poplsb(b), pos.occAll) & ~own);
            mg += sign * mob * 1; eg += sign * mob * 2; }

        // threats: non-pawn pieces attacked by enemy pawns are in danger.
        // (queen's penalty was previously 0 here - a queen sitting on a
        // pawn-attacked square is at least as urgent a problem as a rook
        // being attacked, arguably more since it's harder to just let it
        // be taken; fixed to a real value.)
        {
            static const int thrPen[6] = {0, 28, 28, 45, 70, 0};
            static const int thrPenQueen = 110;
            U64 hit = oppPawnAtt & own & ~myP & ~pos.pieces[c][KING];
            while (hit) {
                int sq = poplsb(hit);
                int p = pos.board[sq];
                int pen = (p == QUEEN) ? thrPenQueen : thrPen[p];
                mg -= sign * pen; eg -= sign * pen * 2 / 3;
            }
        }

        // queen safety: a queen sitting where an enemy minor piece could
        // capture it is a much bigger problem than losing a minor piece,
        // since the follow-up is almost always forced - flag it separately
        // from the general threat table above so it isn't drowned out by
        // the (lower, per-minor-piece) threat values.
        {
            U64 qbb = pos.pieces[c][QUEEN];
            if (qbb) {
                int qsq = lsb(qbb);
                U64 minorAtt = 0;
                U64 bb = pos.pieces[c ^ 1][KNIGHT];
                while (bb) minorAtt |= knightAtt[poplsb(bb)];
                bb = pos.pieces[c ^ 1][BISHOP];
                while (bb) minorAtt |= bishopAtt(poplsb(bb), pos.occAll);
                if (minorAtt & bit(qsq)) { mg -= sign * 140; eg -= sign * 90; }
            }
        }

        // opening risk: developing the queen before the minor pieces is a
        // classic beginner mistake (the queen gets kicked around by tempo
        // gaining developing moves) - penalize it directly instead of
        // relying on search to rediscover this every game. Both checks are
        // purely piece-position based (no ply/move-counter needed), so
        // they naturally fade out as material comes off via the mg/eg
        // phase blend at the end of evaluate().
        {
            int homeRank = c == WHITE ? 0 : 56;
            bool queenHome = pos.pieces[c][QUEEN] & bit(homeRank + 3);
            if (!queenHome) {
                U64 homeMinors = (bit(homeRank + 1) | bit(homeRank + 2) |
                                   bit(homeRank + 5) | bit(homeRank + 6));
                int undeveloped = popcnt((pos.pieces[c][KNIGHT] | pos.pieces[c][BISHOP])
                                          & homeMinors);
                if (undeveloped >= 2) mg -= sign * 16 * undeveloped;
            }
            // kingside/queenside pawn shield disturbed while castling
            // rights for that side are still held - "don't weaken the side
            // you're about to castle into before you've castled"
            bool canOO  = pos.castle & (c == WHITE ? 1 : 4);
            bool canOOO = pos.castle & (c == WHITE ? 2 : 8);
            int pawnStartRank = c == WHITE ? 1 : 6;
            U64 pawnHomeRank = RANK_1 << (pawnStartRank * 8);
            if (canOO) {
                U64 kSideFiles = fileMask[5] | fileMask[6] | fileMask[7];
                int moved = 3 - popcnt(myP & pawnHomeRank & kSideFiles);
                mg -= sign * moved * 10;
            }
            if (canOOO) {
                U64 qSideFiles = fileMask[0] | fileMask[1] | fileMask[2];
                int moved = 3 - popcnt(myP & pawnHomeRank & qSideFiles);
                // queenside shield matters less than kingside in practice
                // (the king ends up on c-file, b/a pawns are less critical)
                mg -= sign * moved * 4;
            }
        }

        // king safety (middlegame): pawn shield + enemy attacks on the king ring
        {
            int kf = myK % 8, kr = myK / 8;
            int shield = 0;
            for (int df = -1; df <= 1; df++) {
                int f = kf + df;
                if (f < 0 || f > 7) continue;
                U64 fp = myP & fileMask[f];
                int r1 = c == WHITE ? kr + 1 : kr - 1;
                int r2 = c == WHITE ? kr + 2 : kr - 2;
                if      (r1 >= 0 && r1 < 8 && (fp & bit(r1 * 8 + f))) shield += 12;
                else if (r2 >= 0 && r2 < 8 && (fp & bit(r2 * 8 + f))) shield += 5;
                if (!fp)                          shield -= 12;  // half-open next to king
                if (!(fp | (oppP & fileMask[f]))) shield -= 6;   // fully open
            }
            mg += sign * shield;

            U64 ring = kingAtt[myK];
            int danger = 0, attackers = 0;
            U64 bb = pos.pieces[c ^ 1][KNIGHT];
            while (bb) { U64 a = knightAtt[poplsb(bb)] & ring;
                if (a) { attackers++; danger += 20 * popcnt(a); } }
            bb = pos.pieces[c ^ 1][BISHOP];
            while (bb) { U64 a = bishopAtt(poplsb(bb), pos.occAll) & ring;
                if (a) { attackers++; danger += 20 * popcnt(a); } }
            bb = pos.pieces[c ^ 1][ROOK];
            while (bb) { U64 a = rookAtt(poplsb(bb), pos.occAll) & ring;
                if (a) { attackers++; danger += 35 * popcnt(a); } }
            int qsq = -1;
            bb = pos.pieces[c ^ 1][QUEEN];
            while (bb) { int sq = poplsb(bb); U64 a = queenAtt(sq, pos.occAll) & ring;
                if (a) { attackers++; danger += 55 * popcnt(a); qsq = sq; } }
            // an enemy rook/queen already lined up on a half-open file next
            // to our king is a much bigger threat than the shield penalty
            // above implies on its own - a sacrifice to fully open that
            // file is usually just a matter of technique from there
            for (int df = -1; df <= 1; df++) {
                int f = kf + df;
                if (f < 0 || f > 7 || (myP & fileMask[f])) continue;
                U64 enemyRQ = (pos.pieces[c ^ 1][ROOK] | pos.pieces[c ^ 1][QUEEN]) & fileMask[f];
                if (enemyRQ) danger += 15;
            }
            // queen making contact with the king ring is an immediate,
            // concrete mating threat rather than a slow buildup.  The old
            // non-linear attacker table gave a lone queen 0% danger, which
            // made Qh6/Qg7-style attacking setups too cheap to allow in the
            // uploaded 2400 games.  Keep the scaled multi-piece danger, but
            // add a modest *base* penalty for queen ring pressure so a lone
            // queen already inside the danger zone is respected.
            int queenRingPressure = 0;
            if (qsq >= 0) {
                U64 qa = queenAtt(qsq, pos.occAll) & ring;
                queenRingPressure += 14 * popcnt(qa);
                if (kingAtt[qsq] & ring) queenRingPressure += 22;
                if (queenRingPressure) {
                    mg -= sign * queenRingPressure;
                    eg -= sign * (queenRingPressure / 2);
                }
                if (kingAtt[qsq] & ring) danger += 25;
            }

            // non-linear scale-up by attacker count: a lone attacker is
            // rarely dangerous (real engines routinely shrug these off),
            // but as pieces coordinate the danger compounds fast - this is
            // precisely the shape that makes a piece sacrifice to add one
            // more attacker (or to rip open a file/diagonal) pay for
            // itself once enough pressure has built up.
            static const int attackPct[8] = {0, 0, 25, 50, 80, 100, 100, 100};
            int pct = attackPct[attackers > 7 ? 7 : attackers];
            mg -= sign * (danger * pct) / 100;

            // queen infiltration: an enemy queen sitting deep in our own
            // camp (our 1st/2nd rank) is dangerous even before it formally
            // joins the king-ring attack count above - it's already
            // forking pawns, cutting off king escape squares, and setting
            // up back-rank/mating threats a ply or two before they land.
            if (qsq >= 0) {
                int qRankFromMyBack = c == WHITE ? qsq / 8 : 7 - qsq / 8;
                if (qRankFromMyBack <= 1) {
                    int qFileDistToKing = abs(qsq % 8 - kf);
                    int infiltPenalty = qFileDistToKing <= 3 ? 40 : 20;
                    mg -= sign * infiltPenalty; eg -= sign * infiltPenalty / 2;
                }
            }

            // mate-threat proxy: count squares our king could actually
            // step to (empty or capturable, and not itself covered by an
            // enemy piece). This is cheap relative to a real search, but
            // catches the "king is almost out of squares" shape that
            // precedes back-rank/smothered-style mates several plies
            // before search would otherwise notice at shallow depth.
            {
                U64 escape = kingAtt[myK] & ~own;
                int freeSquares = 0;
                U64 esc = escape;
                while (esc) {
                    int sq = poplsb(esc);
                    if (!sqAttacked(pos, sq, c ^ 1)) freeSquares++;
                }
                if (attackers >= 2 && freeSquares <= 1) {
                    int mateThreat = (2 - freeSquares) * 60;
                    mg -= sign * mateThreat; eg -= sign * mateThreat;
                }
            }

            // back-rank stability: king stuck on the home rank, walled in
            // by its own pawns with no escape square, and nothing of ours
            // defending that rank while the enemy has a rook/queen aiming
            // down an open or semi-open file at it - the textbook back-rank
            // mate setup, worth flagging well before it's forced.
            {
                int backRank = c == WHITE ? 0 : 7;
                if (kr == backRank) {
                    U64 aheadSq = kingAtt[myK] & ~(RANK_1 << (backRank * 8));
                    bool walledIn = (aheadSq & ~pos.occAll) == 0;  // no empty step forward
                    if (walledIn) {
                        U64 backRankMask = RANK_1 << (backRank * 8);
                        bool defended = (pos.pieces[c][ROOK] | pos.pieces[c][QUEEN]) & backRankMask;
                        U64 enemyRQ = pos.pieces[c ^ 1][ROOK] | pos.pieces[c ^ 1][QUEEN];
                        bool threatened = false;
                        U64 er = enemyRQ;
                        while (er) {
                            int esq = poplsb(er);
                            if (rookAtt(esq, pos.occAll) & backRankMask & bit(myK)) threatened = true;
                        }
                        if (threatened && !defended) { mg -= sign * 50; eg -= sign * 30; }
                    }
                }
            }
        }
    }
    if (phase > 24) phase = 24;
    int score = (mg * phase + eg * (24 - phase)) / 24;
    // tempo: small bonus for the side to move
    return (pos.side == WHITE ? score : -score) + 10;
}


// ---------------- FIRST_NET NNUE (incremental accumulator DEV) ----------------
// Frozen Phase 2 contract:
//   features: 32 king buckets * 12 planes * 64 squares = 24576
//   hidden:   [White 256][Black 256][context 6]
//   context:  STM, phase, WK, WQ, BK, BQ (scale Q=256)
//   output:   White POV cp, rounded half away from zero, then converted to STM POV
//
// Production evaluation uses per-position incremental accumulators.  The
// original full-refresh implementation remains available as a bit-exact
// oracle through whiteCpScratch(), nnuecheck, and nnueverify.
namespace FirstNet {
    static constexpr int INPUT_FEATURES = 24576;
    static constexpr int HIDDEN = FIRSTNET_HIDDEN;
    static constexpr int CONTEXTS = 6;
    static constexpr int OUTPUT_INPUTS = 518;
    static constexpr int Q = 256;
    static constexpr int CP_LIMIT = 30000;

    static constexpr uint64_t HEADER_BYTES = 192;
    static constexpr uint64_t DIRECTORY_BYTES = 256;
    static constexpr uint64_t FT_WEIGHT_COUNT = uint64_t(INPUT_FEATURES) * HIDDEN;
    static constexpr uint64_t FT_WEIGHT_BYTES = FT_WEIGHT_COUNT * sizeof(int16_t);
    static constexpr uint64_t FT_BIAS_BYTES = uint64_t(HIDDEN) * sizeof(int32_t);
    static constexpr uint64_t OUT_WEIGHT_BYTES = uint64_t(OUTPUT_INPUTS) * sizeof(int16_t);
    static constexpr uint64_t OUT_BIAS_BYTES = sizeof(int32_t);
    static constexpr uint64_t PAYLOAD_BYTES =
        FT_WEIGHT_BYTES + FT_BIAS_BYTES + OUT_WEIGHT_BYTES + OUT_BIAS_BYTES;
    static constexpr uint64_t FILE_BYTES = HEADER_BYTES + DIRECTORY_BYTES + PAYLOAD_BYTES;

    struct Network {
        vector<int16_t> ftWeight;
        array<int32_t, HIDDEN> ftBias{};
        array<int16_t, OUTPUT_INPUTS> outWeight{};
        int32_t outBias = 0;
        bool loaded = false;
        string loadedPath;
        string lastError;

        void clear() {
            vector<int16_t>().swap(ftWeight);
            ftBias.fill(0);
            outWeight.fill(0);
            outBias = 0;
            loaded = false;
            loadedPath.clear();
        }
    };

    struct Telemetry {
        uint64_t inferenceCalls = 0;
        uint64_t scratchCalls = 0;
        uint64_t accumulatorCopies = 0;
        uint64_t deltaRows = 0;
        uint64_t refreshes[2] = {0, 0};
        uint64_t kingRefreshes = 0;
        uint64_t invalidations = 0;
    };

    static Network net;
    static Telemetry telemetry;
    static bool useNNUE = true;
    static int blendPercent = 35;   // default hybrid: 65% Classical + 35% FIRST_NET
    static bool hybridGuard = true; // reduce NNUE trust on large disagreements/endgames
    static string evalFile = "firstnet_v3.snnue";
    static filesystem::path exeDirectory;
    static uint64_t networkGeneration = 1;

    static bool hotEnabled() {
        return useNNUE && blendPercent > 0 && net.loaded;
    }

    static uint32_t readLE32(const unsigned char *p) {
        return uint32_t(p[0]) |
               (uint32_t(p[1]) << 8) |
               (uint32_t(p[2]) << 16) |
               (uint32_t(p[3]) << 24);
    }

    static uint64_t readLE64(const unsigned char *p) {
        uint64_t value = 0;
        for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
        return value;
    }

    static bool readExact(ifstream &stream, void *dst, size_t bytes) {
        stream.read(reinterpret_cast<char *>(dst), static_cast<streamsize>(bytes));
        return stream.good() || (stream.eof() && static_cast<size_t>(stream.gcount()) == bytes);
    }

    static filesystem::path resolvePath(const string &name) {
        filesystem::path direct(name);
        if (direct.is_absolute() || filesystem::exists(direct)) return direct;
        if (!exeDirectory.empty()) {
            filesystem::path besideExe = exeDirectory / direct;
            if (filesystem::exists(besideExe)) return besideExe;
        }
        return direct;
    }

    static bool load(const string &name, bool verbose) {
        net.clear();
        net.lastError.clear();
        filesystem::path path = resolvePath(name);

        error_code ec;
        uint64_t size = filesystem::file_size(path, ec);
        if (ec) {
            net.lastError = "cannot open " + path.string();
            if (verbose) printf("info string NNUE load failed: %s\n", net.lastError.c_str());
            return false;
        }
        if (size != FILE_BYTES) {
            net.lastError = "bad file size " + to_string(size) + " expected " + to_string(FILE_BYTES);
            if (verbose) printf("info string NNUE load failed: %s\n", net.lastError.c_str());
            return false;
        }

        ifstream stream(path, ios::binary);
        if (!stream) {
            net.lastError = "cannot open " + path.string();
            if (verbose) printf("info string NNUE load failed: %s\n", net.lastError.c_str());
            return false;
        }

        array<unsigned char, HEADER_BYTES> header{};
        if (!readExact(stream, header.data(), header.size())) {
            net.lastError = "truncated header";
            if (verbose) printf("info string NNUE load failed: %s\n", net.lastError.c_str());
            return false;
        }

        // Layout: <8s16I2Q32s32s32sQ
        const uint32_t headerBytes = readLE32(header.data() + 12);
        const uint32_t kingBuckets = readLE32(header.data() + 28);
        const uint32_t hidden = readLE32(header.data() + 36);
        const uint32_t q = readLE32(header.data() + 60);
        const uint32_t tensorCount = readLE32(header.data() + 64);
        const uint32_t tensorEntryBytes = readLE32(header.data() + 68);
        const uint64_t directoryBytes = readLE64(header.data() + 72);
        const uint64_t payloadBytes = readLE64(header.data() + 80);

        if (headerBytes != HEADER_BYTES || kingBuckets != 32 || hidden != HIDDEN ||
            q != Q || tensorCount != 4 || tensorEntryBytes != 64 ||
            directoryBytes != DIRECTORY_BYTES || payloadBytes != PAYLOAD_BYTES) {
            net.lastError = "incompatible SNNUE v2 header";
            if (verbose) printf("info string NNUE load failed: %s\n", net.lastError.c_str());
            return false;
        }

        stream.seekg(static_cast<streamoff>(HEADER_BYTES + DIRECTORY_BYTES), ios::beg);
        if (!stream) {
            net.lastError = "cannot seek to payload";
            if (verbose) printf("info string NNUE load failed: %s\n", net.lastError.c_str());
            return false;
        }

        try {
            net.ftWeight.resize(static_cast<size_t>(FT_WEIGHT_COUNT));
        } catch (const bad_alloc &) {
            net.lastError = "not enough memory for FT weights";
            if (verbose) printf("info string NNUE load failed: %s\n", net.lastError.c_str());
            return false;
        }

        if (!readExact(stream, net.ftWeight.data(), static_cast<size_t>(FT_WEIGHT_BYTES)) ||
            !readExact(stream, net.ftBias.data(), static_cast<size_t>(FT_BIAS_BYTES)) ||
            !readExact(stream, net.outWeight.data(), static_cast<size_t>(OUT_WEIGHT_BYTES)) ||
            !readExact(stream, &net.outBias, static_cast<size_t>(OUT_BIAS_BYTES))) {
            net.clear();
            net.lastError = "truncated tensor payload";
            if (verbose) printf("info string NNUE load failed: %s\n", net.lastError.c_str());
            return false;
        }

        net.loaded = true;
        ++networkGeneration;
        if (networkGeneration == 0) ++networkGeneration;
        net.loadedPath = filesystem::absolute(path, ec).string();
        if (ec) net.loadedPath = path.string();
        if (verbose) {
            printf("info string NNUE loaded: %s (%llu bytes) generation=%llu\n",
                   net.loadedPath.c_str(), static_cast<unsigned long long>(size),
                   static_cast<unsigned long long>(networkGeneration));
        }
        return true;
    }

    static int roundAwayDiv(int64_t numerator, int64_t denominator) {
        if (numerator >= 0)
            return static_cast<int>((numerator + denominator / 2) / denominator);
        return -static_cast<int>((-numerator + denominator / 2) / denominator);
    }

    static int normalizedSquare(int square, int perspective, int kingSquare) {
        int verticalKing = perspective == WHITE ? kingSquare : (kingSquare ^ 56);
        int mirror = (verticalKing & 7) >= 4 ? 7 : 0;
        int verticalPiece = perspective == WHITE ? square : (square ^ 56);
        return verticalPiece ^ mirror;
    }

    static int featureIndex(int pieceSquare, int pieceColor, int pieceType,
                            int perspective, int kingSquare) {
        int normalizedKing = normalizedSquare(kingSquare, perspective, kingSquare);
        int bucket = (normalizedKing >> 3) * 4 + (normalizedKing & 7);
        int plane = (pieceColor ^ perspective) * 6 + pieceType;
        int normalizedPiece = normalizedSquare(pieceSquare, perspective, kingSquare);
        return ((bucket * 12 + plane) * 64) + normalizedPiece;
    }

    static int phaseContext(const Position &pos) {
        int phase = 0;
        phase += popcnt(pos.pieces[WHITE][KNIGHT]) + popcnt(pos.pieces[BLACK][KNIGHT]);
        phase += popcnt(pos.pieces[WHITE][BISHOP]) + popcnt(pos.pieces[BLACK][BISHOP]);
        phase += 2 * (popcnt(pos.pieces[WHITE][ROOK]) + popcnt(pos.pieces[BLACK][ROOK]));
        phase += 4 * (popcnt(pos.pieces[WHITE][QUEEN]) + popcnt(pos.pieces[BLACK][QUEEN]));
        phase = clamp(phase, 0, 24);
        return roundAwayDiv(int64_t(phase) * Q, 24);
    }

    static void invalidate(Position &pos) {
        if (pos.nnueValidMask) ++telemetry.invalidations;
        pos.nnueValidMask = 0;
        pos.nnueGeneration = networkGeneration;
    }

    static void prepareGeneration(Position &pos) {
        if (pos.nnueGeneration != networkGeneration) {
            pos.nnueValidMask = 0;
            pos.nnueGeneration = networkGeneration;
            ++telemetry.invalidations;
        }
    }

    static void addRow(array<int32_t, HIDDEN> &acc, int feature, int sign) {
        const int16_t *row = &net.ftWeight[static_cast<size_t>(feature) * HIDDEN];
        if (sign > 0) {
            for (int h = 0; h < HIDDEN; ++h) acc[h] += row[h];
        } else {
            for (int h = 0; h < HIDDEN; ++h) acc[h] -= row[h];
        }
        ++telemetry.deltaRows;
    }

    static void scratchAccumulator(const Position &pos, int perspective,
                                   array<int32_t, HIDDEN> &out) {
        for (int h = 0; h < HIDDEN; ++h) out[h] = net.ftBias[h];
        const int kingSquare = lsb(pos.pieces[perspective][KING]);
        for (int color = WHITE; color <= BLACK; ++color) {
            for (int piece = PAWN; piece <= KING; ++piece) {
                U64 bb = pos.pieces[color][piece];
                while (bb) {
                    int square = poplsb(bb);
                    int feature = featureIndex(square, color, piece, perspective, kingSquare);
                    const int16_t *row = &net.ftWeight[static_cast<size_t>(feature) * HIDDEN];
                    for (int h = 0; h < HIDDEN; ++h) out[h] += row[h];
                }
            }
        }
    }

    static void refreshPerspective(Position &pos, int perspective, bool kingMove) {
        if (!hotEnabled() || !pos.pieces[perspective][KING]) {
            pos.nnueValidMask &= uint8_t(~(1u << perspective));
            return;
        }
        prepareGeneration(pos);
        scratchAccumulator(pos, perspective, pos.nnueAcc[perspective]);
        pos.nnueValidMask |= uint8_t(1u << perspective);
        ++telemetry.refreshes[perspective];
        if (kingMove) ++telemetry.kingRefreshes;
    }

    static void ensure(Position &pos) {
        if (!hotEnabled()) {
            invalidate(pos);
            return;
        }
        prepareGeneration(pos);
        if (!(pos.nnueValidMask & 1)) refreshPerspective(pos, WHITE, false);
        if (!(pos.nnueValidMask & 2)) refreshPerspective(pos, BLACK, false);
    }

    static void onPieceAdd(Position &pos, int color, int piece, int square) {
        if (!hotEnabled()) { invalidate(pos); return; }
        prepareGeneration(pos);
        for (int perspective = WHITE; perspective <= BLACK; ++perspective) {
            if (!(pos.nnueValidMask & (1u << perspective))) continue;
            int kingSquare = lsb(pos.pieces[perspective][KING]);
            addRow(pos.nnueAcc[perspective],
                   featureIndex(square, color, piece, perspective, kingSquare), +1);
        }
    }

    static void onPieceRemove(Position &pos, int color, int piece, int square) {
        if (!hotEnabled()) { invalidate(pos); return; }
        prepareGeneration(pos);
        for (int perspective = WHITE; perspective <= BLACK; ++perspective) {
            if (!(pos.nnueValidMask & (1u << perspective))) continue;
            int kingSquare = lsb(pos.pieces[perspective][KING]);
            addRow(pos.nnueAcc[perspective],
                   featureIndex(square, color, piece, perspective, kingSquare), -1);
        }
    }

    static void onPieceMove(Position &pos, int color, int piece, int from, int to) {
        if (!hotEnabled()) { invalidate(pos); return; }
        prepareGeneration(pos);
        for (int perspective = WHITE; perspective <= BLACK; ++perspective) {
            if (!(pos.nnueValidMask & (1u << perspective))) continue;
            if (piece == KING && perspective == color) {
                // Every feature index depends on this perspective king.
                pos.nnueValidMask &= uint8_t(~(1u << perspective));
                continue;
            }
            int kingSquare = lsb(pos.pieces[perspective][KING]);
            addRow(pos.nnueAcc[perspective],
                   featureIndex(from, color, piece, perspective, kingSquare), -1);
            addRow(pos.nnueAcc[perspective],
                   featureIndex(to, color, piece, perspective, kingSquare), +1);
        }
    }

    static void finishLegalMove(Position &pos, int movingColor, int movedPiece) {
        if (!hotEnabled()) { invalidate(pos); return; }
        prepareGeneration(pos);
        if (movedPiece == KING && !(pos.nnueValidMask & (1u << movingColor)))
            refreshPerspective(pos, movingColor, true);
    }

    static int rawFromAccumulators(const Position &pos,
                                   const array<int32_t, HIDDEN> &whiteAcc,
                                   const array<int32_t, HIDDEN> &blackAcc) {
        int64_t raw = net.outBias;
        for (int h = 0; h < HIDDEN; ++h) {
            int64_t w = clamp<int64_t>(whiteAcc[h], 0, Q);
            int64_t b = clamp<int64_t>(blackAcc[h], 0, Q);
            raw += w * net.outWeight[h];
            raw += b * net.outWeight[HIDDEN + h];
        }
        const int context[CONTEXTS] = {
            pos.side == WHITE ? Q : -Q,
            phaseContext(pos),
            (pos.castle & 1) ? Q : 0,
            (pos.castle & 2) ? Q : 0,
            (pos.castle & 4) ? Q : 0,
            (pos.castle & 8) ? Q : 0,
        };
        for (int lane = 0; lane < CONTEXTS; ++lane)
            raw += int64_t(context[lane]) * net.outWeight[2 * HIDDEN + lane];
        return clamp(roundAwayDiv(raw, Q), -CP_LIMIT, CP_LIMIT);
    }

    static int whiteCpScratch(const Position &pos) {
        if (!net.loaded || !pos.pieces[WHITE][KING] || !pos.pieces[BLACK][KING]) return 0;
        ++telemetry.scratchCalls;
        array<int32_t, HIDDEN> whiteAcc{}, blackAcc{};
        scratchAccumulator(pos, WHITE, whiteAcc);
        scratchAccumulator(pos, BLACK, blackAcc);
        return rawFromAccumulators(pos, whiteAcc, blackAcc);
    }

    static int whiteCp(const Position &pos) {
        if (!net.loaded || !pos.pieces[WHITE][KING] || !pos.pieces[BLACK][KING]) return 0;
        Position &mutablePos = const_cast<Position &>(pos);
        ensure(mutablePos);
        if ((mutablePos.nnueValidMask & 3) != 3) return whiteCpScratch(pos);
        ++telemetry.inferenceCalls;
        return rawFromAccumulators(pos, mutablePos.nnueAcc[WHITE], mutablePos.nnueAcc[BLACK]);
    }

    static int sideToMoveCp(const Position &pos) {
        int score = whiteCp(pos);
        return pos.side == WHITE ? score : -score;
    }

    static bool exactPosition(const Position &pos, bool verbose) {
        if (!net.loaded || !pos.pieces[WHITE][KING] || !pos.pieces[BLACK][KING]) return false;
        Position &mutablePos = const_cast<Position &>(pos);
        ensure(mutablePos);
        array<int32_t, HIDDEN> expected[2];
        scratchAccumulator(pos, WHITE, expected[WHITE]);
        scratchAccumulator(pos, BLACK, expected[BLACK]);
        int mismatches = 0;
        for (int perspective = WHITE; perspective <= BLACK; ++perspective) {
            for (int h = 0; h < HIDDEN; ++h) {
                if (mutablePos.nnueAcc[perspective][h] != expected[perspective][h]) {
                    if (verbose && mismatches < 8)
                        printf("NNUE neuron mismatch p=%d h=%d inc=%lld scratch=%lld\n",
                               perspective, h,
                               static_cast<long long>(mutablePos.nnueAcc[perspective][h]),
                               static_cast<long long>(expected[perspective][h]));
                    ++mismatches;
                }
            }
        }
        int inc = rawFromAccumulators(pos, mutablePos.nnueAcc[WHITE], mutablePos.nnueAcc[BLACK]);
        int scratch = rawFromAccumulators(pos, expected[WHITE], expected[BLACK]);
        if (inc != scratch) {
            if (verbose) printf("NNUE score mismatch inc=%d scratch=%d\n", inc, scratch);
            ++mismatches;
        }
        if (verbose)
            printf("NNUE exact: %s  neurons=%d  whiteCp=%d\n",
                   mismatches ? "FAIL" : "PASS", 2 * HIDDEN, inc);
        return mismatches == 0;
    }

    static bool trackingActive(const Position &pos) {
        return hotEnabled() && pos.nnueGeneration == networkGeneration && pos.nnueValidMask != 0;
    }

    static void noteAccumulatorCopy() { ++telemetry.accumulatorCopies; }

    static void resetTelemetry() { telemetry = Telemetry{}; }

    static void printTelemetry() {
        printf("NNUE telemetry: infer=%llu scratch=%llu copies=%llu deltaRows=%llu "
               "refreshW=%llu refreshB=%llu kingRefresh=%llu invalidations=%llu\n",
               static_cast<unsigned long long>(telemetry.inferenceCalls),
               static_cast<unsigned long long>(telemetry.scratchCalls),
               static_cast<unsigned long long>(telemetry.accumulatorCopies),
               static_cast<unsigned long long>(telemetry.deltaRows),
               static_cast<unsigned long long>(telemetry.refreshes[WHITE]),
               static_cast<unsigned long long>(telemetry.refreshes[BLACK]),
               static_cast<unsigned long long>(telemetry.kingRefreshes),
               static_cast<unsigned long long>(telemetry.invalidations));
    }
}

static int nnueTestMove(const string &uci) {
    if (uci.size() < 4) return 0;
    int fromFile = uci[0] - 'a', fromRank = uci[1] - '1';
    int toFile = uci[2] - 'a', toRank = uci[3] - '1';
    if (fromFile < 0 || fromFile > 7 || toFile < 0 || toFile > 7 ||
        fromRank < 0 || fromRank > 7 || toRank < 0 || toRank > 7) return 0;
    int promo = 0;
    if (uci.size() >= 5) {
        switch (tolower(static_cast<unsigned char>(uci[4]))) {
            case 'n': promo = KNIGHT; break;
            case 'b': promo = BISHOP; break;
            case 'r': promo = ROOK; break;
            case 'q': promo = QUEEN; break;
            default: return 0;
        }
    }
    return makeMv(fromRank * 8 + fromFile, toRank * 8 + toFile, promo);
}

static bool runNNUEFixture(const char *name, const char *fen, const char *uci, bool verbose) {
    Position p;
    if (!setFen(p, fen)) {
        if (verbose) printf("NNUE fixture %-24s FAIL bad FEN\n", name);
        return false;
    }
    if (!FirstNet::exactPosition(p, false)) {
        if (verbose) printf("NNUE fixture %-24s FAIL initial\n", name);
        return false;
    }
    int move = nnueTestMove(uci);
    if (!move || !makeMove(p, move)) {
        if (verbose) printf("NNUE fixture %-24s FAIL move %s\n", name, uci);
        return false;
    }
    bool afterMake = FirstNet::exactPosition(p, false);
    unmakeMove(p);
    bool afterUnmake = FirstNet::exactPosition(p, false);
    bool ok = afterMake && afterUnmake;
    if (verbose) printf("NNUE fixture %-24s %s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

static bool runNNUEWhitebox(bool verbose) {
    struct Fixture { const char *name, *fen, *move; };
    static const Fixture fixtures[] = {
        {"quiet pawn", START_FEN, "e2e4"},
        {"knight move", START_FEN, "g1f3"},
        {"white king d/e boundary", "7k/8/8/8/8/8/4K3/8 w - - 0 1", "e2d2"},
        {"black king d/e boundary", "8/4k3/8/8/8/8/8/K7 b - - 0 1", "e7d7"},
        {"white O-O", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1"},
        {"white O-O-O", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1c1"},
        {"black O-O", "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", "e8g8"},
        {"black O-O-O", "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", "e8c8"},
        {"white en passant", "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5d6"},
        {"black en passant", "4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 1", "e4d3"},
        {"white promote queen", "7k/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8q"},
        {"white promote rook", "7k/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8r"},
        {"white promote bishop", "7k/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8b"},
        {"white promote knight", "7k/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8n"},
        {"black promote queen", "4k3/8/8/8/8/8/p7/7K b - - 0 1", "a2a1q"},
        {"black promote rook", "4k3/8/8/8/8/8/p7/7K b - - 0 1", "a2a1r"},
        {"black promote bishop", "4k3/8/8/8/8/8/p7/7K b - - 0 1", "a2a1b"},
        {"black promote knight", "4k3/8/8/8/8/8/p7/7K b - - 0 1", "a2a1n"},
        {"white capture promotion", "1r5k/P7/8/8/8/8/8/4K3 w - - 0 1", "a7b8q"},
        {"black capture promotion", "4k3/8/8/8/8/8/p7/1R5K b - - 0 1", "a2b1q"},
    };
    int passed = 0;
    for (const auto &fixture : fixtures)
        if (runNNUEFixture(fixture.name, fixture.fen, fixture.move, verbose)) ++passed;

    // Null move: feature accumulators must remain byte-for-byte unchanged.
    Position p;
    bool nullOk = setFen(p, START_FEN) && FirstNet::exactPosition(p, false);
    if (nullOk) {
        auto w = p.nnueAcc[WHITE];
        auto b = p.nnueAcc[BLACK];
        uint8_t mask = p.nnueValidMask;
        uint64_t gen = p.nnueGeneration;
        NullUndo nu;
        makeNull(p, nu);
        nullOk = (p.nnueAcc[WHITE] == w && p.nnueAcc[BLACK] == b &&
                  p.nnueValidMask == mask && p.nnueGeneration == gen &&
                  FirstNet::exactPosition(p, false));
        unmakeNull(p, nu);
        nullOk = nullOk && (p.nnueAcc[WHITE] == w && p.nnueAcc[BLACK] == b &&
                            FirstNet::exactPosition(p, false));
    }
    if (verbose) printf("NNUE fixture %-24s %s\n", "null move", nullOk ? "PASS" : "FAIL");
    if (nullOk) ++passed;

    const int total = static_cast<int>(sizeof(fixtures) / sizeof(fixtures[0])) + 1;
    if (verbose) printf("NNUE whitebox: %d/%d %s\n", passed, total,
                        passed == total ? "PASS" : "FAIL");
    return passed == total;
}

static uint64_t nnueVerifyRng = 0x534E495045523233ULL;
static uint64_t nnueVerifyRand() {
    nnueVerifyRng ^= nnueVerifyRng >> 12;
    nnueVerifyRng ^= nnueVerifyRng << 25;
    nnueVerifyRng ^= nnueVerifyRng >> 27;
    return nnueVerifyRng * 0x2545F4914F6CDD1DULL;
}

static bool runNNUERandomVerify(int targetPlies, bool verbose) {
    if (!FirstNet::net.loaded) {
        if (verbose) printf("NNUE random verify: FAIL network not loaded\n");
        return false;
    }
    targetPlies = max(1, targetPlies);
    nnueVerifyRng = 0x534E495045523233ULL;
    int checked = 0, sequences = 0;
    while (checked < targetPlies) {
        Position p;
        if (!setFen(p, START_FEN) || !FirstNet::exactPosition(p, false)) return false;
        int made = 0;
        int limit = 24 + static_cast<int>(nnueVerifyRand() % 73);
        for (int ply = 0; ply < limit && checked < targetPlies; ++ply) {
            MoveList ml;
            genMoves(p, ml, false);
            if (!ml.cnt) break;
            int startIndex = static_cast<int>(nnueVerifyRand() % ml.cnt);
            bool moved = false;
            for (int j = 0; j < ml.cnt; ++j) {
                int m = ml.moves[(startIndex + j) % ml.cnt];
                if (makeMove(p, m)) {
                    moved = true;
                    ++made;
                    ++checked;
                    if (!FirstNet::exactPosition(p, false)) {
                        if (verbose)
                            printf("NNUE random verify: FAIL after make seq=%d ply=%d move=%s\n",
                                   sequences, ply, mvToStr(m).c_str());
                        return false;
                    }
                    break;
                }
            }
            if (!moved) break;
        }
        while (made > 0) {
            unmakeMove(p);
            --made;
            if (!FirstNet::exactPosition(p, false)) {
                if (verbose)
                    printf("NNUE random verify: FAIL after unmake seq=%d remaining=%d\n",
                           sequences, made);
                return false;
            }
        }
        ++sequences;
        if (sequences > targetPlies * 2) break;
    }
    if (verbose)
        printf("NNUE random verify: PASS sequences=%d checkedPlies=%d seed=0x534E495045523233\n",
               sequences, checked);
    return checked >= targetPlies;
}

static int effectiveNNUEWeight(const Position &pos, int classical, int nnue) {
    int weight = clamp(FirstNet::blendPercent, 0, 100);
    if (!FirstNet::hybridGuard || weight <= 0 || weight >= 100) return weight;

    // FIRST_NET v3 is useful as a positional correction, but the match logs
    // showed that large optimistic disagreements can persist for many plies.
    // Keep Classical as the safety anchor whenever the two evaluators strongly
    // disagree instead of averaging a likely outlier at full configured weight.
    const int disagreement = abs(classical - nnue);
    if (disagreement >= 500)      weight = weight * 30 / 100;
    else if (disagreement >= 300) weight = weight * 50 / 100;
    else if (disagreement >= 180) weight = weight * 75 / 100;

    // Validation error was much larger in late middlegames/endgames than in
    // openings. phaseContext is 256 near the opening and 0 in bare endgames.
    const int phaseQ = FirstNet::phaseContext(pos);
    if (phaseQ < 64)       weight = weight * 60 / 100;
    else if (phaseQ < 128) weight = weight * 80 / 100;

    return clamp(weight, 0, 100);
}

static int nnuePruningUncertainty() {
    if (!FirstNet::useNNUE || !FirstNet::net.loaded || FirstNet::blendPercent <= 0)
        return 0;
    // At the default 35% blend this adds 70 cp of safety to static-eval
    // pruning. Pure Classical remains bit-for-bit unchanged.
    return clamp(FirstNet::blendPercent * 2, 0, 200);
}

static int evaluate(const Position &pos) {
    if (!FirstNet::useNNUE || !FirstNet::net.loaded || FirstNet::blendPercent <= 0)
        return evaluateClassical(pos);

    const int nnue = FirstNet::sideToMoveCp(pos);
    if (FirstNet::blendPercent >= 100) return nnue; // explicit pure-NNUE test mode

    const int classical = evaluateClassical(pos);
    const int weight = effectiveNNUEWeight(pos, classical, nnue);
    return FirstNet::roundAwayDiv(
        int64_t(classical) * (100 - weight) + int64_t(nnue) * weight,
        100
    );
}

// ---------------- static exchange evaluation (SEE) ----------------
// Estimates the material outcome of a capture sequence on one square.
// Used to prune losing captures in qsearch and to detect sacrifices.
static U64 attackersTo(const Position &pos, int sq, U64 occ) {
    U64 knights = pos.pieces[WHITE][KNIGHT] | pos.pieces[BLACK][KNIGHT];
    U64 kings   = pos.pieces[WHITE][KING]   | pos.pieces[BLACK][KING];
    U64 bq = pos.pieces[WHITE][BISHOP] | pos.pieces[BLACK][BISHOP]
           | pos.pieces[WHITE][QUEEN]  | pos.pieces[BLACK][QUEEN];
    U64 rq = pos.pieces[WHITE][ROOK]   | pos.pieces[BLACK][ROOK]
           | pos.pieces[WHITE][QUEEN]  | pos.pieces[BLACK][QUEEN];
    return (pawnAtt[BLACK][sq] & pos.pieces[WHITE][PAWN])
         | (pawnAtt[WHITE][sq] & pos.pieces[BLACK][PAWN])
         | (knightAtt[sq] & knights)
         | (kingAtt[sq]   & kings)
         | (bishopAtt(sq, occ) & bq)
         | (rookAtt(sq, occ)   & rq);
}
static int see(const Position &pos, int m) {
    static const int sv[7] = {100, 320, 330, 500, 900, 20000, 0};
    int from = mvFrom(m), to = mvTo(m);
    int gain[32], d = 0;
    U64 occ = pos.occAll;
    int attacker = pos.board[from];
    int stm = pos.side;
    int captured = pos.board[to];
    if (attacker == PAWN && captured == NO_PIECE && (to & 7) != (from & 7))
        captured = PAWN;  // en passant (occupancy shift of the removed pawn ignored)
    gain[0] = captured == NO_PIECE ? 0 : sv[captured];
    U64 fromSet = bit(from);
    do {
        d++;
        gain[d] = sv[attacker] - gain[d - 1];       // speculative: we get captured back
        if (max(-gain[d - 1], gain[d]) < 0) break;  // prune: neither side can improve
        occ ^= fromSet;                              // remove attacker from occupancy
        U64 attadef = attackersTo(pos, to, occ) & occ;  // recompute -> x-rays revealed
        stm ^= 1;
        fromSet = 0;
        for (int p = PAWN; p <= KING; p++) {         // least valuable attacker first
            U64 s = attadef & pos.pieces[stm][p];
            if (s) { fromSet = bit(lsb(s)); attacker = p; break; }
        }
    } while (fromSet);
    while (--d) gain[d - 1] = -max(-gain[d - 1], gain[d]);
    return gain[0];
}

// ---------------- transposition table ----------------
enum { TT_EXACT, TT_LOWER, TT_UPPER };
struct TTE { U64 key; int32_t score; uint16_t move; int8_t depth; uint8_t flag; };
static vector<TTE> tt;
static size_t ttMask;
static int configuredHashMb = 64;
static void ttInit(size_t mb = 64) {
    size_t n = (mb * 1024 * 1024) / sizeof(TTE);
    size_t sz = 1; while (sz * 2 <= n) sz *= 2;
    tt.assign(sz, TTE{0, 0, 0, 0, 0});
    ttMask = sz - 1;
}
static TTE *ttProbe(U64 key) {
    TTE &e = tt[key & ttMask];
    return e.key == key ? &e : nullptr;
}
static void ttStore(U64 key, int score, int move, int depth, int flag, int ply) {
    TTE &e = tt[key & ttMask];
    if (e.key == key && e.depth > depth && flag != TT_EXACT) return;
    if (score >  MATE - 256) score += ply;
    if (score < -MATE + 256) score -= ply;
    e.key = key; e.score = score; e.move = (uint16_t)move;
    e.depth = (int8_t)depth; e.flag = (uint8_t)flag;
}

// ---------------- search ----------------
static long long nodes;
static long long stopTime;
static bool timeLimited, stopped;
static int killers[128][2];
static int historyTbl[2][64][64];
static int rootBestMove;
static int rootExclude = 0;        // coach: exclude this move at root (second-best search)
static int lmrTbl[64][64];         // late move reduction amounts
static void initLmr() {
    for (int d = 1; d < 64; d++)
        for (int m = 1; m < 64; m++)
            lmrTbl[d][m] = (int)(0.5 + log((double)d) * log((double)m) / 2.4);
}
static int lastScore, lastDepth;   // last completed search result (side-to-move view)

static void checkTime() {
    if (timeLimited && nowMs() >= stopTime) stopped = true;
}

static bool isRepetition(const Position &pos) {
    int cnt = 0;
    for (int i = repIdx - 2; i >= 0 && i >= repIdx - pos.halfmove - 1; i -= 2)
        if (repKeys[i] == pos.key && ++cnt >= 1) return true;  // twofold in search
    return false;
}

// cheap, approximate "does this move give check" test used only for move
// ordering (ignores discovered checks and en passant - a false negative
// here just means a checking move gets ordered a bit later, not a bug).
static inline bool givesCheckApprox(const Position &pos, int m) {
    int us = pos.side, them = us ^ 1;
    int to = mvTo(m), from = mvFrom(m);
    int piece = pos.board[from];
    int promo = mvPromo(m);
    if (promo) piece = promo;
    int ksq = lsb(pos.pieces[them][KING]);
    switch (piece) {
        case PAWN:   return pawnAtt[us][to] & bit(ksq);
        case KNIGHT: return knightAtt[to] & bit(ksq);
        case BISHOP: return bishopAtt(to, pos.occAll) & bit(ksq);
        case ROOK:   return rookAtt(to, pos.occAll) & bit(ksq);
        case QUEEN:  return queenAtt(to, pos.occAll) & bit(ksq);
        default:     return false;
    }
}


static inline bool isAdvancedPawnMove(const Position &pos, int m) {
    int from = mvFrom(m), to = mvTo(m);
    if (!sqOk(from) || pos.board[from] != PAWN) return false;
    int r = to / 8;
    // Moving a pawn onto the 7th rank (or promoting) is tactically volatile.
    // Earlier match logs were full of promotion-race positions, so do not let
    // LMR/futility treat these as sleepy quiet moves.
    return mvPromo(m) || (pos.side == WHITE ? r >= 6 : r <= 1);
}

static inline bool touchesEnemyKingRing(const Position &pos, int m) {
    int enemyK = lsb(pos.pieces[pos.side ^ 1][KING]);
    return (kingAtt[enemyK] & bit(mvTo(m))) != 0;
}

static void scoreMoves(const Position &pos, MoveList &ml, int ttMove, int ply) {
    int enemyK = lsb(pos.pieces[pos.side ^ 1][KING]);
    U64 kRing = kingAtt[enemyK];
    for (int i = 0; i < ml.cnt; i++) {
        int m = ml.moves[i];
        if (m == ttMove) { ml.score[i] = 1 << 22; continue; }
        int to = mvTo(m), from = mvFrom(m);
        int cap = pos.board[to];
        if (cap == NO_PIECE && pos.board[from] == PAWN && (to & 7) != (from & 7))
            cap = PAWN;  // en passant
        // nudge checks and moves into the enemy king ring earlier in the
        // move order, so attacking/sacrificial lines get searched (and
        // deepened via the check extension) before the time budget runs
        // out, instead of being buried behind quiet developing moves
        int atkBonus = 0;
        if (givesCheckApprox(pos, m))    atkBonus = 300;
        else if (bit(to) & kRing)        atkBonus = 60;
        if (cap != NO_PIECE) {
            // SearchBlade: order captures by SEE first, not only MVV/LVA.
            // This pushes safe tactics and sacrifices with real compensation up,
            // while burying obvious losing captures behind forcing quiet moves.
            int seeScore = see(pos, m);
            ml.score[i] = (1 << 20) + cap * 1000 - pos.board[from] * 10 + seeScore * 4 + atkBonus;
        }
        else if (mvPromo(m))
            ml.score[i] = (1 << 20) + mvPromo(m) * 1000 + atkBonus;
        else if (m == killers[ply][0]) ml.score[i] = (1 << 19) + atkBonus;
        else if (m == killers[ply][1]) ml.score[i] = (1 << 19) - 1 + atkBonus;
        else ml.score[i] = historyTbl[pos.side][from][to] + atkBonus;
    }
}
static void pickMove(MoveList &ml, int i) {
    int best = i;
    for (int j = i + 1; j < ml.cnt; j++)
        if (ml.score[j] > ml.score[best]) best = j;
    swap(ml.moves[i], ml.moves[best]);
    swap(ml.score[i], ml.score[best]);
}

static int qsearch(Position &pos, int alpha, int beta, int ply) {
    nodes++;
    if ((nodes & 4095) == 0) checkTime();
    if (stopped) return 0;
    if (ply >= 120) return evaluate(pos);

    bool chk = inCheck(pos);
    int stand = 0;
    if (!chk) {
        stand = evaluate(pos);
        if (stand >= beta) return stand;
        if (stand > alpha) alpha = stand;
    }
    MoveList ml;
    // SearchBlade: normal qsearch sees captures/promotions; at the first few
    // qsearch plies it also admits quiet checking moves.  This catches many
    // short forcing mates without turning the whole search into full-width
    // chaos.  Quiet checks are exactly the kind of move that made the 2400 /
    // Carlsen wins look sharp, so they should not disappear at the horizon.
    bool includeQuietChecks = !chk && ply <= 3;
    genMoves(pos, ml, chk || includeQuietChecks ? false : true);
    scoreMoves(pos, ml, 0, ply < 128 ? ply : 127);
    int legal = 0, best = -INF;
    for (int i = 0; i < ml.cnt; i++) {
        pickMove(ml, i);
        int m = ml.moves[i];
        int from = mvFrom(m), to = mvTo(m);
        int cap = pos.board[to];
        bool epCap = (cap == NO_PIECE && pos.board[from] == PAWN && (to & 7) != (from & 7));
        bool isCap = (cap != NO_PIECE) || epCap;
        bool isPromo = mvPromo(m) != 0;
        bool checkish = givesCheckApprox(pos, m);
        if (!chk && !isCap && !isPromo && !checkish) continue;
        if (!chk && isCap && !isPromo) {
            if (cap == NO_PIECE) cap = PAWN;  // en passant
            // delta pruning: even winning this piece can't reach alpha
            if (stand + pieceVal[cap] + 200 <= alpha) continue;
            // SEE pruning: skip captures that lose material
            if (see(pos, m) < 0) continue;
        }
        if (!makeMove(pos, m)) continue;
        legal++;
        int sc = -qsearch(pos, -beta, -alpha, ply + 1);
        unmakeMove(pos);
        if (stopped) return 0;
        if (sc > best) best = sc;
        if (sc > alpha) { alpha = sc; if (alpha >= beta) break; }
    }
    if (chk && legal == 0) return -MATE + ply;
    if (!chk && best == -INF) return alpha;
    return best > alpha ? best : alpha;
}

static inline int drawScoreForSideToMove(const Position &pos) {
    // Draw contempt, but only as a tiny local preference.  If the side to
    // move is clearly better, repetition/50-move draw is slightly worse than
    // continuing; if clearly worse, drawing is slightly attractive.  This is
    // aimed at the 2400 match draws where SniperBishop held +2~+5 for a long
    // time and then repeated into a half point.
    int ev = evaluate(pos);
    if (ev > 220) return -28;
    if (ev > 110) return -12;
    if (ev < -220) return 28;
    if (ev < -110) return 12;
    return 0;
}

static int search(Position &pos, int alpha, int beta, int depth, int ply, bool doNull) {
    if (depth <= 0) return qsearch(pos, alpha, beta, ply);
    // SAFETY: check extensions have no cumulative cap, so a line with many
    // consecutive checks (e.g. a losing side spite-checking) could in
    // theory keep re-extending depth faster than it decrements, recursing
    // far deeper than any normal iterative-deepening depth would suggest.
    // Each search() frame holds a 2KB+ MoveList on the stack, so unbounded
    // recursion risks a real stack overflow - especially on Windows, where
    // the default thread stack (1MB, smaller still in Debug builds) is far
    // less forgiving than typical Linux defaults. A stack overflow doesn't
    // always crash cleanly; it can silently corrupt a caller's local
    // variables (like bestMove), which looks exactly like the engine
    // "choosing" a nonsensical move. This hard cap makes that impossible.
    if (ply >= 100) return qsearch(pos, alpha, beta, ply);
    nodes++;
    if ((nodes & 4095) == 0) checkTime();
    if (stopped) return 0;

    bool root = (ply == 0);
    bool pvNode = (beta - alpha > 1);

    if (!root) {
        if (pos.halfmove >= 100 || isRepetition(pos)) return drawScoreForSideToMove(pos);
        // mate distance pruning
        int mateAlpha = -MATE + ply, mateBeta = MATE - ply - 1;
        if (alpha < mateAlpha) alpha = mateAlpha;
        if (beta  > mateBeta)  beta  = mateBeta;
        if (alpha >= beta) return alpha;
    }

    bool chk = inCheck(pos);
    if (chk) depth++;  // check extension
    int staticEval = chk ? -INF : evaluate(pos);

    // TT probe
    int ttMove = 0;
    TTE *e = ttProbe(pos.key);
    if (e) {
        ttMove = e->move;
        if (!root && !pvNode && e->depth >= depth) {
            int sc = e->score;
            if (sc >  MATE - 256) sc -= ply;
            if (sc < -MATE + 256) sc += ply;
            if (e->flag == TT_EXACT) return sc;
            if (e->flag == TT_LOWER && sc >= beta)  return sc;
            if (e->flag == TT_UPPER && sc <= alpha) return sc;
        }
    }

    // reverse futility pruning: static eval is so far above beta that
    // even a large positional swing won't bring it back down
    const int evalUncertainty = nnuePruningUncertainty();

    if (!pvNode && !chk && depth <= 6 && abs(beta) < MATE - 256 &&
        staticEval - (85 * depth + evalUncertainty) >= beta)
        return staticEval;

    // null-move pruning
    U64 bigPieces = pos.pieces[pos.side][KNIGHT] | pos.pieces[pos.side][BISHOP] |
                    pos.pieces[pos.side][ROOK]   | pos.pieces[pos.side][QUEEN];
    if (!pvNode && doNull && !chk && depth >= 3 && bigPieces &&
        staticEval >= beta + evalUncertainty / 2) {
        NullUndo nu;
        makeNull(pos, nu);
        int R = 2 + depth / 6;
        int sc = -search(pos, -beta, -beta + 1, depth - 1 - R, ply + 1, false);
        unmakeNull(pos, nu);
        if (stopped) return 0;
        if (sc >= beta && sc < MATE - 256) return beta;
    }

    MoveList ml;
    genMoves(pos, ml, false);
    scoreMoves(pos, ml, ttMove, ply < 128 ? ply : 127);

    int legal = 0, bestScore = -INF, bestMove = 0;
    int oldAlpha = alpha;

    // futility: at low depth, when static eval is hopelessly below alpha,
    // quiet moves are very unlikely to help
    bool futile = !pvNode && !chk && depth <= 4 && abs(alpha) < MATE - 256 &&
                  staticEval + 90 * depth + 80 + evalUncertainty <= alpha;

    for (int i = 0; i < ml.cnt; i++) {
        pickMove(ml, i);
        int m = ml.moves[i];
        if (root && rootExclude && m == rootExclude) continue;  // second-best search
        int from = mvFrom(m), to = mvTo(m);
        bool quiet = (pos.board[to] == NO_PIECE) && !mvPromo(m) &&
                     !(pos.board[from] == PAWN && (to & 7) != (from & 7));
        bool checkish = givesCheckApprox(pos, m);
        bool kingRingMove = touchesEnemyKingRing(pos, m);
        bool advancedPawn = isAdvancedPawnMove(pos, m);
        // SearchBlade: extend near-promotion pawn pushes in real search.
        // The match logs show that passed-pawn races and promotion attacks are
        // one of SniperBishop's biggest weapons, so give those lines one more
        // ply instead of relying on qsearch to rescue them at the edge.
        bool recapture = (undoSp > 0 && pos.board[to] != NO_PIECE &&
                          mvTo(undoStack[undoSp - 1].move) == to);
        int ext = (advancedPawn && depth >= 3 && ply < 80) ? 1 : 0;
        // EloBoost: tactical quiet threats and immediate recaptures often need
        // one more ply.  This especially helps human-like "deep-only" moves:
        // quiet king-ring entries, passed-pawn pushes, and forcing recaptures.
        if (recapture && depth >= 4 && ply < 80) ext = max(ext, 1);
        if (quiet && kingRingMove && depth >= 5 && legal <= 10 && ply < 80) ext = max(ext, 1);
        if (futile && quiet && legal > 0 && !checkish && !kingRingMove && !advancedPawn) continue;
        if (!makeMove(pos, m)) continue;
        legal++;

        int sc;
        if (legal == 1) {
            sc = -search(pos, -beta, -alpha, depth - 1 + ext, ply + 1, true);
        } else {
            // late move reduction (log-scaled by depth and move count). Do not
            // reduce quiet moves that are actually tactical: checks, king-ring
            // entries, and near-promotion pawn pushes were exactly where the
            // uploaded games kept producing decisive tactics.
            int red = 0;
            if (depth >= 3 && legal > 3 && quiet && !chk &&
                !checkish && !kingRingMove && !advancedPawn && !recapture) {
                red = lmrTbl[depth < 64 ? depth : 63][legal < 64 ? legal : 63];
                if (pvNode && red) red--;
                if (red >= depth) red = depth - 1;
            }
            sc = -search(pos, -alpha - 1, -alpha, depth - 1 - red + ext, ply + 1, true);
            if (sc > alpha && red)
                sc = -search(pos, -alpha - 1, -alpha, depth - 1 + ext, ply + 1, true);
            if (sc > alpha && sc < beta)
                sc = -search(pos, -beta, -alpha, depth - 1 + ext, ply + 1, true);
        }
        unmakeMove(pos);
        if (stopped) return 0;

        if (sc > bestScore) {
            bestScore = sc; bestMove = m;
            if (root) rootBestMove = m;
        }
        if (sc > alpha) {
            alpha = sc;
            if (alpha >= beta) {
                if (quiet) {
                    int kply = ply < 128 ? ply : 127;
                    if (killers[kply][0] != m) {
                        killers[kply][1] = killers[kply][0];
                        killers[kply][0] = m;
                    }
                    historyTbl[pos.side][from][to] += depth * depth;
                    if (historyTbl[pos.side][from][to] > (1 << 18))
                        for (int c = 0; c < 2; c++)
                            for (int a = 0; a < 64; a++)
                                for (int b2 = 0; b2 < 64; b2++)
                                    historyTbl[c][a][b2] /= 2;
                }
                break;
            }
        }
    }

    if (legal == 0) return chk ? -MATE + ply : 0;

    int flag = bestScore >= beta ? TT_LOWER :
               bestScore > oldAlpha ? TT_EXACT : TT_UPPER;
    if (!(root && rootExclude))   // don't poison the TT during second-best searches
        ttStore(pos.key, bestScore, bestMove, depth, flag, ply);
    return bestScore;
}


static bool isLegalMoveNow(Position &pos, int m) {
    MoveList ml; genMoves(pos, ml, false);
    for (int i = 0; i < ml.cnt; i++) {
        if (ml.moves[i] != m) continue;
        if (makeMove(pos, m)) { unmakeMove(pos); return true; }
        return false;
    }
    return false;
}

// PV extraction from TT
static string getPV(Position &pos, int maxLen) {
    string pv;
    vector<int> made;
    // BUGFIX: this was `U64 seen[64]`, but maxLen = depth + 8 and depth can
    // reach the UCI "go" handler's default maxDepth of 64, so maxLen could
    // reach 72 - overflowing a 64-slot array by writing past its end on the
    // stack. This is exactly the kind of corruption that can silently
    // clobber an unrelated local variable in the caller and produce a
    // nonsensical move with no crash to explain it. Sized generously and
    // defensively clamped below so this can never happen again regardless
    // of future maxDepth changes.
    static const int SEEN_CAP = 256;
    U64 seen[SEEN_CAP]; int seenCnt = 0;
    if (maxLen > SEEN_CAP) maxLen = SEEN_CAP;
    for (int i = 0; i < maxLen; i++) {
        TTE *e = ttProbe(pos.key);
        if (!e || !e->move) break;
        bool rep = false;
        for (int j = 0; j < seenCnt; j++) if (seen[j] == pos.key) rep = true;
        if (rep) break;
        seen[seenCnt++] = pos.key;
        // verify legality
        MoveList ml; genMoves(pos, ml, false);
        bool found = false;
        for (int j = 0; j < ml.cnt; j++)
            if (ml.moves[j] == e->move) { found = true; break; }
        if (!found) break;
        if (!makeMove(pos, e->move)) break;
        made.push_back(e->move);
        pv += mvToStr(e->move) + " ";
    }
    for (int i = (int)made.size() - 1; i >= 0; i--) unmakeMove(pos);
    return pv;
}

// iterative deepening
static int think(Position &pos, int maxDepth, long long timeMs, bool print) {
    nodes = 0; stopped = false;
    timeLimited = timeMs > 0;
    long long start = nowMs();
    stopTime = start + (timeMs > 0 ? timeMs : 1LL << 60);
    memset(killers, 0, sizeof(killers));
    rootBestMove = 0;

    int bestMove = 0, prevScore = 0;
    for (int d = 1; d <= maxDepth; d++) {
        int alpha = -INF, beta = INF;
        if (d >= 5) { alpha = prevScore - 30; beta = prevScore + 30; }
        int sc;
        for (;;) {
            sc = search(pos, alpha, beta, d, 0, true);
            if (stopped) break;
            if (sc <= alpha)      { alpha = max(-INF, alpha - 120); }
            else if (sc >= beta)  { beta  = min( INF, beta  + 120); }
            else break;
        }
        if (stopped) break;
        prevScore = sc;
        lastScore = sc; lastDepth = d;
        bestMove = rootBestMove;
        long long el = nowMs() - start;
        if (print) {
            // PV is rebuilt from TT using the same makeMove() legality gate as
            // real play. Since king-captures are now impossible, PV extraction
            // cannot walk into a missing-king / lsb(0) corrupted position.
            string pv = getPV(pos, d + 8);
            if (abs(sc) > MATE - 256) {
                int mate = (MATE - abs(sc) + 1) / 2;
                if (!pv.empty())
                    printf("info depth %d score mate %d nodes %lld nps %lld time %lld pv %s\n",
                           d, sc > 0 ? mate : -mate, nodes,
                           el > 0 ? nodes * 1000 / el : 0, el, pv.c_str());
                else
                    printf("info depth %d score mate %d nodes %lld nps %lld time %lld\n",
                           d, sc > 0 ? mate : -mate, nodes,
                           el > 0 ? nodes * 1000 / el : 0, el);
            } else {
                if (!pv.empty())
                    printf("info depth %d score cp %d nodes %lld nps %lld time %lld pv %s\n",
                           d, sc, nodes, el > 0 ? nodes * 1000 / el : 0, el, pv.c_str());
                else
                    printf("info depth %d score cp %d nodes %lld nps %lld time %lld\n",
                           d, sc, nodes, el > 0 ? nodes * 1000 / el : 0, el);
            }
            fflush(stdout);
        }
        // don't start a new iteration that likely won't finish
        if (timeLimited && (nowMs() - start) * 100 > timeMs * 55) break;
        if (abs(sc) > MATE - 256) break;
    }
    if (!bestMove) {  // fallback: any legal move
        MoveList ml; genMoves(pos, ml, false);
        for (int i = 0; i < ml.cnt; i++)
            if (makeMove(pos, ml.moves[i])) { unmakeMove(pos); bestMove = ml.moves[i]; break; }
    }
    return bestMove;
}

// coach: best score when a given move is excluded at the root
// (used to detect "Great" moves - the only good move in the position)
static int thinkSecond(Position &pos, long long ms, int exclude) {
    int svScore = lastScore, svDepth = lastDepth;
    rootExclude = exclude;
    think(pos, 64, ms, false);
    rootExclude = 0;
    int sc = lastScore;
    lastScore = svScore; lastDepth = svDepth;
    return sc;
}

// ---------------- perft ----------------
static U64 perft(Position &pos, int depth) {
    if (depth == 0) return 1;
    MoveList ml;
    genMoves(pos, ml, false);
    U64 total = 0;
    for (int i = 0; i < ml.cnt; i++) {
        if (!makeMove(pos, ml.moves[i])) continue;
        total += perft(pos, depth - 1);
        unmakeMove(pos);
    }
    return total;
}

// ---------------- opening book ----------------
struct BookLine { const char *name; const char *moves; };
static const BookLine BOOK[] = {
    {"Ruy Lopez",               "e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1 f8e7"},
    {"Italian Game",            "e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 c2c3 g8f6 d2d3 d7d6"},
    {"Sicilian Najdorf",        "e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6"},
    {"Sicilian Sveshnikov",     "e2e4 c7c5 g1f3 b8c6 d2d4 c5d4 f3d4 g8f6 b1c3 e7e5"},
    {"French Defense",          "e2e4 e7e6 d2d4 d7d5 b1c3 g8f6 c1g5 f8e7 e4e5 f6d7"},
    {"Caro-Kann Defense",       "e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5 e4g3 f5g6"},
    {"Queen's Gambit Declined", "d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7 e2e3 e8g8"},
    {"Slav Defense",            "d2d4 d7d5 c2c4 c7c6 g1f3 g8f6 b1c3 d5c4 a2a4 c8f5"},
    {"Nimzo-Indian Defense",    "d2d4 g8f6 c2c4 e7e6 b1c3 f8b4 e2e3 e8g8 f1d3 d7d5"},
    {"King's Indian Defense",   "d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6 g1f3 e8g8"},
    {"English Opening",         "c2c4 e7e5 b1c3 g8f6 g1f3 b8c6 g2g3 d7d5 c4d5 f6d5"},
    {"Reti Opening",            "g1f3 d7d5 d2d4 g8f6 c2c4 e7e6 b1c3 f8e7 c1g5 e8g8"},
};
static vector<string> splitStr(const char *s) {
    istringstream ss(s); vector<string> v; string t;
    while (ss >> t) v.push_back(t);
    return v;
}
static string bookMove(const vector<string> &played) {
    vector<string> cands;
    for (const BookLine &bl : BOOK) {
        vector<string> mv = splitStr(bl.moves);
        if (played.size() >= mv.size()) continue;
        bool match = true;
        for (size_t i = 0; i < played.size(); i++)
            if (mv[i] != played[i]) { match = false; break; }
        if (match) cands.push_back(mv[played.size()]);
    }
    if (cands.empty()) return "";
    return cands[rand64() % cands.size()];
}
// returns the opening name once the played moves match exactly one book line
static string openingName(const vector<string> &played) {
    if (played.empty()) return "";
    string name; int matches = 0;
    for (const BookLine &bl : BOOK) {
        vector<string> mv = splitStr(bl.moves);
        size_t n = min(mv.size(), played.size());
        bool match = true;
        for (size_t i = 0; i < n; i++)
            if (mv[i] != played[i]) { match = false; break; }
        if (match) { matches++; name = bl.name; }
    }
    return matches == 1 ? name : "";
}
// is this move known opening theory in the current line?
static bool bookHas(const vector<string> &played, const string &mv) {
    for (const BookLine &bl : BOOK) {
        vector<string> m = splitStr(bl.moves);
        if (played.size() >= m.size()) continue;
        bool match = true;
        for (size_t i = 0; i < played.size(); i++)
            if (m[i] != played[i]) { match = false; break; }
        if (match && m[played.size()] == mv) return true;
    }
    return false;
}

// ---------------- move quality rating (coach) ----------------
// chess.com-style classification of a played move, based on the eval of
// the engine's best move before the move vs the eval after the move.
enum { C_BOOK, C_FORCED, C_BRILLIANT, C_GREAT, C_BEST, C_EXCELLENT,
       C_GOOD, C_NORMAL, C_INACC, C_MISTAKE, C_BLUNDER, C_MISSED, C_N };
static const char *catName[C_N] = {
    "Book", "Forced", "Brilliant", "Great", "Best", "Excellent",
    "Good", "Normal", "Inaccuracy", "Mistake", "Blunder", "Missed"};
static const char *catSym[C_N] = {
    "", "", "!!", "!", "", "", "", "", "?!", "?", "??", "?"};

static int classifyMove(bool book, bool forced, bool isBest, bool deepOnly, bool sac,
                        bool forcing, bool haveSecond, int secondSc, int preSc, int postSc) {
    if (book)   return C_BOOK;
    if (forced) return C_FORCED;

    // Missed = the deep engine had a decisive move/mate, but the played move
    // did not preserve it.  This is intentionally checked before generic
    // blunder buckets so a missed mate is labelled as Missed, not merely ?? .
    if (!isBest && preSc > MATE - 256 && postSc <= MATE - 256) return C_MISSED;
    if (!isBest && preSc >= 650 && postSc < 250 && preSc - postSc >= 350) return C_MISSED;

    // CoachFix rule:
    //   Best      = the deep analysis engine's best move.
    //   Great     = shallow search missed it, deep search finds it.
    //   Brilliant = same deep-only idea, but with sacrifice/forcing/mate flavor.
    // In other words, Brilliant/Great are NOT just "the eval went up".
    if (isBest) {
        if (deepOnly) {
            if ((sac && postSc >= -80) || forcing || postSc > MATE - 512) return C_BRILLIANT;
            if (haveSecond && secondSc <= preSc - 120) return C_BRILLIANT;
            return C_GREAT;
        }
        return C_BEST;
    }

    int loss = preSc - postSc;
    if (loss < 0) loss = 0;
    if (loss <= 20)  return C_EXCELLENT;
    if (loss <= 60)  return C_GOOD;
    if (loss <= 100) return C_NORMAL;
    if (loss <= 160) return C_INACC;
    if (loss <= 300) return C_MISTAKE;
    return C_BLUNDER;
}

// win probability / per-move accuracy (lichess formulas)
static double winPct(int cp) {
    if (cp >  1000) cp =  1000;
    if (cp < -1000) cp = -1000;
    return 50.0 + 50.0 * (2.0 / (1.0 + exp(-0.00368208 * cp)) - 1.0);
}
static double moveAccuracy(int preSc, int postSc) {
    double d = winPct(preSc) - winPct(postSc);
    if (d < 0) d = 0;
    double a = 103.1668 * exp(-0.04354 * d) - 3.1669;
    return a > 100 ? 100 : (a < 0 ? 0 : a);
}

// ---------------- move parsing ----------------
static int promoFromChar(char ch) {
    switch (tolower((unsigned char)ch)) {
        case 'n': return KNIGHT;
        case 'b': return BISHOP;
        case 'r': return ROOK;
        case 'q': return QUEEN;
        default:  return 0;
    }
}

static bool parseUciSquares(const string &s, int &from, int &to, int &promo) {
    if (s.size() != 4 && s.size() != 5) return false;
    char f1 = (char)tolower((unsigned char)s[0]);
    char r1 = s[1];
    char f2 = (char)tolower((unsigned char)s[2]);
    char r2 = s[3];
    if (f1 < 'a' || f1 > 'h' || f2 < 'a' || f2 > 'h' ||
        r1 < '1' || r1 > '8' || r2 < '1' || r2 > '8') return false;
    from = (r1 - '1') * 8 + (f1 - 'a');
    to   = (r2 - '1') * 8 + (f2 - 'a');
    promo = 0;
    if (s.size() == 5) {
        promo = promoFromChar(s[4]);
        if (!promo) return false;
    }
    return true;
}

static int parseMove(Position &pos, const string &raw) {
    string s = raw;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t k = 0; while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) k++;
    if (k) s = s.substr(k);
    if (s.size() == 5) s[4] = (char)tolower((unsigned char)s[4]);

    int from = -1, to = -1, promo = 0;
    if (!parseUciSquares(s, from, to, promo)) return 0;

    // IMPORTANT: match UCI input by decoded from/to/promotion fields, not by
    // comparing against mvToStr(). The old promotion-output bug also broke
    // input parsing: a GUI move like f7f8q failed to parse if our internal
    // queen promotion string was incorrectly rendered as f7f8r. That silently
    // desynced the engine from cutechess. This parser is independent of output.
    MoveList ml;
    genMoves(pos, ml, false);
    for (int i = 0; i < ml.cnt; i++) {
        int m = ml.moves[i];
        if (mvFrom(m) != from || mvTo(m) != to) continue;
        if (mvPromo(m) != promo) continue;
        if (makeMove(pos, m)) { unmakeMove(pos); return m; }
    }
    return 0;
}

// ---------------- game state helpers ----------------
static int countLegal(Position &pos) {
    MoveList ml; genMoves(pos, ml, false);
    int n = 0;
    for (int i = 0; i < ml.cnt; i++)
        if (makeMove(pos, ml.moves[i])) { unmakeMove(pos); n++; }
    return n;
}

static string sqName(int sq) {
    string s; s += (char)('a' + sq % 8); s += (char)('1' + sq / 8);
    return s;
}
static string trimStr(string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

// SAN generation: Nf3, exd5, O-O, e8=Q, +, #
static string sanOf(Position &pos, int m) {
    int from = mvFrom(m), to = mvTo(m), promo = mvPromo(m);
    int piece = pos.board[from];
    string san;
    if (piece == KING && to - from == 2)       san = "O-O";
    else if (piece == KING && from - to == 2)  san = "O-O-O";
    else {
        bool cap = pos.board[to] != NO_PIECE ||
                   (piece == PAWN && (to & 7) != (from & 7));
        if (piece == PAWN) {
            if (cap) { san += (char)('a' + from % 8); san += 'x'; }
            san += sqName(to);
            if (promo) { san += '='; san += (char)toupper((unsigned char)promoChar(promo)); }
        } else {
            san += "PNBRQK"[piece];
            // disambiguate when another identical piece can reach the same square
            MoveList ml; genMoves(pos, ml, false);
            bool amb = false, sameFile = false, sameRank = false;
            for (int i = 0; i < ml.cnt; i++) {
                int m2 = ml.moves[i];
                if (m2 == m || mvTo(m2) != to) continue;
                if (pos.board[mvFrom(m2)] != piece) continue;
                if (!makeMove(pos, m2)) continue;
                unmakeMove(pos);
                amb = true;
                if (mvFrom(m2) % 8 == from % 8) sameFile = true;
                if (mvFrom(m2) / 8 == from / 8) sameRank = true;
            }
            if (amb) {
                if (!sameFile)      san += (char)('a' + from % 8);
                else if (!sameRank) san += (char)('1' + from / 8);
                else { san += (char)('a' + from % 8); san += (char)('1' + from / 8); }
            }
            if (cap) san += 'x';
            san += sqName(to);
        }
    }
    if (makeMove(pos, m)) {
        if (inCheck(pos)) san += (countLegal(pos) == 0) ? '#' : '+';
        unmakeMove(pos);
    }
    return san;
}

// SAN input parsing (coordinate input is handled by parseMove)
static int parseSAN(Position &pos, string s) {
    s = trimStr(s);
    while (!s.empty() && strchr("+#!?", s.back())) s.pop_back();
    if (s == "0-0" || s == "o-o" || s == "OO")   s = "O-O";
    if (s == "0-0-0" || s == "o-o-o" || s == "OOO") s = "O-O-O";
    if (s.empty()) return 0;
    MoveList ml; genMoves(pos, ml, false);
    for (int i = 0; i < ml.cnt; i++) {
        int m = ml.moves[i];
        if (!makeMove(pos, m)) continue;
        unmakeMove(pos);
        string t = sanOf(pos, m);
        while (!t.empty() && strchr("+#", t.back())) t.pop_back();
        if (t == s) return m;
    }
    return 0;
}

// eval score string (white-view centipawns -> "+0.35" / "mate in N")
static string evalStr(int cpWhite) {
    char buf[48];
    if (cpWhite >  MATE - 256)
        snprintf(buf, 48, "mate in %d for White", (MATE - cpWhite + 1) / 2);
    else if (cpWhite < -MATE + 256)
        snprintf(buf, 48, "mate in %d for Black", (MATE + cpWhite + 1) / 2);
    else
        snprintf(buf, 48, "%+.2f", cpWhite / 100.0);
    return buf;
}

static int verifyOrFallback(Position &pos, int bm, const char *whereFrom);
// ---------------- interactive play ----------------
// current position -> FEN string
static string posToFen(const Position &pos, int fullmove) {
    static const char pc[2][6] = {{'P','N','B','R','Q','K'},{'p','n','b','r','q','k'}};
    string fen;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            int c = colorAt(pos, sq);
            if (c < 0) { empty++; continue; }
            if (empty) { fen += (char)('0' + empty); empty = 0; }
            fen += pc[c][pos.board[sq]];
        }
        if (empty) fen += (char)('0' + empty);
        if (r) fen += '/';
    }
    fen += pos.side == WHITE ? " w " : " b ";
    string cast;
    if (pos.castle & 1) cast += 'K';
    if (pos.castle & 2) cast += 'Q';
    if (pos.castle & 4) cast += 'k';
    if (pos.castle & 8) cast += 'q';
    fen += cast.empty() ? "-" : cast;
    fen += ' ';
    fen += pos.ep >= 0 ? sqName(pos.ep) : "-";
    char tail[24];
    snprintf(tail, 24, " %d %d", pos.halfmove, fullmove);
    fen += tail;
    return fen;
}

// captured pieces + material balance line (derived from piece counts,
// clamped so promotions never show negative captures)
static void printStatus(const Position &pos, const vector<string> &sanHist) {
    static const int startCnt[5] = {8, 2, 2, 2, 1};
    static const char symW[5] = {'P','N','B','R','Q'};
    static const char symB[5] = {'p','n','b','r','q'};
    string wCaps, bCaps;   // pieces captured BY white / BY black
    int matW = 0, matB = 0;
    for (int p = PAWN; p <= QUEEN; p++) {
        int wHave = popcnt(pos.pieces[WHITE][p]);
        int bHave = popcnt(pos.pieces[BLACK][p]);
        matW += pieceVal[p] * wHave;
        matB += pieceVal[p] * bHave;
        for (int i = 0; i < max(0, startCnt[p] - bHave); i++) wCaps += symB[p];
        for (int i = 0; i < max(0, startCnt[p] - wHave); i++) bCaps += symW[p];
    }
    int diff = matW - matB;
    printf("  Captured by White: %-16s  Captured by Black: %s\n",
           wCaps.empty() ? "-" : wCaps.c_str(),
           bCaps.empty() ? "-" : bCaps.c_str());
    if (diff)
        printf("  Material: %s +%.1f\n", diff > 0 ? "White" : "Black", abs(diff) / 100.0);
    // last few moves for context
    if (!sanHist.empty()) {
        printf("  Moves:");
        size_t from = sanHist.size() > 8 ? sanHist.size() - 8 : 0;
        if (from > 0) printf(" ...");
        for (size_t i = from; i < sanHist.size(); i++) {
            if (i % 2 == 0) printf(" %d.", (int)i / 2 + 1);
            printf(" %s", sanHist[i].c_str());
        }
        printf("\n");
    }
}

static void printHistory(const vector<string> &sanHist, const vector<string> &annot) {
    if (sanHist.empty()) { printf("No moves played yet.\n"); return; }
    for (size_t i = 0; i < sanHist.size(); i++) {
        string s = sanHist[i] + (i < annot.size() ? annot[i] : "");
        if (i % 2 == 0) printf("%3d. %-10s", (int)i / 2 + 1, s.c_str());
        else            printf("%s\n", s.c_str());
    }
    if (sanHist.size() % 2) printf("\n");
}

static void printPgn(const vector<string> &sanHist, const vector<string> &annot,
                     int human, const string &result) {
    printf("\n[Event \"Casual Game\"]\n");
    printf("[Site \"Sniper Bishop CLI\"]\n");
    printf("[White \"%s\"]\n", human == WHITE ? "Human" : "Sniper Bishop 2.0");
    printf("[Black \"%s\"]\n", human == BLACK ? "Human" : "Sniper Bishop 2.0");
    printf("[Result \"%s\"]\n\n", result.c_str());
    int col = 0;
    for (size_t i = 0; i < sanHist.size(); i++) {
        string s = sanHist[i] + (i < annot.size() ? annot[i] : "");
        char buf[40];
        if (i % 2 == 0) snprintf(buf, 40, "%d. %s ", (int)i / 2 + 1, s.c_str());
        else            snprintf(buf, 40, "%s ", s.c_str());
        col += (int)strlen(buf);
        if (col > 76) { printf("\n"); col = (int)strlen(buf); }
        printf("%s", buf);
    }
    printf("%s\n\n", result.c_str());
}

static void playHelp() {
    printf("\nHow to enter moves (both formats work):\n");
    printf("  coordinate : e2e4, g1f3, e7e8q (promotion: append q/r/b/n)\n");
    printf("  SAN        : e4, Nf3, exd5, O-O, O-O-O, e8=Q\n");
    printf("Commands:\n");
    printf("  moves    list all legal moves      hint     engine suggestion\n");
    printf("  eval     evaluate the position     undo     take back one move\n");
    printf("  board    redraw the board          history  show full move list\n");
    printf("  pgn      export game as PGN        fen      show current FEN\n");
    printf("  flip     flip the board view       level N  set think time (ms)\n");
    printf("  coach    toggle move ratings       new      restart the game\n");
    printf("  quit     end the game\n\n");
}

static void playMode(Position &pos) {
    setFen(pos, START_FEN);
    vector<string> played;    // coordinate notation (for book matching)
    vector<string> sanHist;   // SAN notation (for display / PGN)
    string line;
    printf("\n===== Sniper Bishop - Play Mode =====\n");

    // color selection
    int human = WHITE;
    for (;;) {
        printf("Choose color - w: White / b: Black / r: random  [Enter=White] : ");
        fflush(stdout);
        if (!getline(cin, line)) return;
        line = trimStr(line);
        if (line.empty() || line[0] == 'w' || line[0] == 'W') { human = WHITE; break; }
        if (line[0] == 'b' || line[0] == 'B') { human = BLACK; break; }
        if (line[0] == 'r' || line[0] == 'R') {
            human = (rand64() ^ (U64)nowMs()) % 2 ? BLACK : WHITE;
            break;
        }
        printf("Please enter w, b, or r.\n");
    }
    printf("-> You play %s.\n", human == WHITE ? "White (you move first)" : "Black");

    // difficulty (think time)
    long long tms = 2000;
    printf("Engine think time in ms - 500: easy / 2000: normal / 5000: hard  [Enter=2000] : ");
    fflush(stdout);
    if (getline(cin, line) && !trimStr(line).empty())
        tms = max(100LL, atoll(trimStr(line).c_str()));
    playHelp();
    printf("Coach is ON: your moves get chess.com-style ratings (type 'coach' to toggle).\n");

    int lastMv = 0;           // for board highlight
    bool flipView = false;    // manual board flip toggle
    string result = "*";      // PGN result
    vector<string> annot;     // per-move rating symbols (!!, ?, ?? ...)

    // ----- coach state -----
    bool coach = true;
    bool analyzed = false;    // pre-move analysis done for this turn?
    int  preBest = 0, preShallowBest = 0, preScore = 0;
    bool havePre = false;
    bool pending = false;     // a played move waiting for its post-move eval
    size_t pendIdx = 0;
    bool pendBook = false, pendForced = false, pendIsBest = false, pendDeepOnly = false,
         pendSac = false, pendForcing = false, pendHaveSecond = false;
    int  pendSecond = 0, pendPreScore = 0;
    string pendBestSan;
    int catCnt[C_N] = {0};
    double accSum = 0; int accN = 0; long long cplSum = 0;

    // classify + report a played human move, given the eval after it
    // (postSc is from the human's point of view)
    auto resolvePending = [&](int postSc) {
        if (!pending) return;
        pending = false;
        int cat = classifyMove(pendBook, pendForced, pendIsBest, pendDeepOnly,
                               pendSac, pendForcing, pendHaveSecond, pendSecond,
                               pendPreScore, postSc);
        catCnt[cat]++;
        if (cat != C_BOOK && cat != C_FORCED) {
            int loss = pendPreScore - postSc;
            if (loss < 0) loss = 0;
            if (loss > 1000) loss = 1000;
            cplSum += loss;
            accSum += moveAccuracy(pendPreScore, postSc);
            accN++;
        }
        if (pendIdx < annot.size()) annot[pendIdx] = catSym[cat];
        string mvTxt = sanHist[pendIdx] + catSym[cat];
        int wPre  = human == WHITE ? pendPreScore : -pendPreScore;
        int wPost = human == WHITE ? postSc : -postSc;
        if (cat == C_BOOK)
            printf("  Move rating: %s - Book (opening theory)\n", mvTxt.c_str());
        else if (cat == C_FORCED)
            printf("  Move rating: %s - Forced (only legal move)\n", mvTxt.c_str());
        else if (cat <= C_BEST)
            printf("  Move rating: %s - %s  (eval %s)\n",
                   mvTxt.c_str(), catName[cat], evalStr(wPost).c_str());
        else
            printf("  Move rating: %s - %s  (best was %s; %s -> %s)\n",
                   mvTxt.c_str(), catName[cat], pendBestSan.c_str(),
                   evalStr(wPre).c_str(), evalStr(wPost).c_str());
    };
    auto printReview = [&]() {
        bool any = false;
        for (int c = 0; c < C_N; c++) if (catCnt[c]) any = true;
        if (!any) return;
        printf("\n--- Game Review (your moves) ---\n");
        for (int c = 0; c < C_N; c++)
            if (catCnt[c]) printf("  %-12s %d\n", catName[c], catCnt[c]);
        if (accN) {
            printf("  %-12s %lld\n", "Avg CP loss", cplSum / accN);
            printf("  %-12s %.1f%%\n", "Accuracy", accSum / accN);
        }
    };
    auto resetGame = [&]() {
        setFen(pos, START_FEN);
        played.clear(); sanHist.clear(); annot.clear();
        lastMv = 0; result = "*";
        analyzed = false; havePre = false; preShallowBest = 0; pending = false;
        memset(catCnt, 0, sizeof(catCnt));
        accSum = 0; accN = 0; cplSum = 0;
    };

    for (;;) {
        int persp = flipView ? (human ^ 1) : human;
        printBoard(pos, persp, lastMv);
        printStatus(pos, sanHist);
        string open = openingName(played);
        if (!open.empty()) printf("  Opening: %s\n", open.c_str());

        int legal = countLegal(pos);
        if (legal == 0) {
            if (inCheck(pos)) {
                resolvePending(MATE);   // pending only exists if the human just moved
                result = pos.side == WHITE ? "0-1" : "1-0";
                printf("\nCheckmate! %s\n", pos.side == human ? "Engine wins!" : "You win!");
            } else {
                resolvePending(0);
                result = "1/2-1/2";
                printf("\nStalemate - draw\n");
            }
            printReview();
            printPgn(sanHist, annot, human, result);
            break;
        }
        if (pos.halfmove >= 100) {
            resolvePending(0);
            result = "1/2-1/2";
            printf("\nFifty-move rule - draw\n");
            printReview();
            printPgn(sanHist, annot, human, result);
            break;
        }
        if (inCheck(pos)) printf("  ** Check! **\n");

        if (pos.side == human) {
            // coach: analyze the position once before you move
            if (coach && !analyzed) {
                printf("  (coach is analyzing...)\n");
                fflush(stdout);
                // CoachFix: shallow/deep comparison.  The shallow pass
                // represents "the engine did not immediately see it"; the
                // deep pass is the actual Best reference.
                long long shallowMs = min(350LL, max(120LL, tms / 4));
                preShallowBest = think(pos, 64, shallowMs, false);
                preBest = think(pos, 64, min(tms, 1400LL), false);
                preScore = lastScore;
                havePre = preBest != 0;
                analyzed = true;
            }
            printf("\n[move %d, %s] > ", (int)played.size() / 2 + 1,
                   pos.side == WHITE ? "White" : "Black");
            fflush(stdout);
            if (!getline(cin, line)) break;
            line = trimStr(line);
            if (line.empty()) continue;
            if (line == "quit" || line == "q") {
                printReview();
                printf("Game ended.\n"); break;
            }
            if (line == "resign") {
                result = human == WHITE ? "0-1" : "1-0";
                printf("You resigned. Engine wins!\n");
                printReview();
                printPgn(sanHist, annot, human, result);
                break;
            }
            if (line == "help" || line == "?") { playHelp(); continue; }
            if (line == "board" || line == "d") continue;
            if (line == "coach") {
                coach = !coach;
                pending = false; analyzed = false; havePre = false; preShallowBest = 0;
                printf("Move ratings %s.\n", coach ? "ON" : "OFF");
                continue;
            }
            if (line == "flip") {
                flipView = !flipView;
                printf("Board view flipped.\n");
                continue;
            }
            if (line == "new") {
                resetGame();
                printf("New game started. You still play %s.\n",
                       human == WHITE ? "White" : "Black");
                continue;
            }
            if (line == "history") { printHistory(sanHist, annot); continue; }
            if (line == "pgn")     { printPgn(sanHist, annot, human, result); continue; }
            if (line == "fen") {
                printf("%s\n", posToFen(pos, (int)played.size() / 2 + 1).c_str());
                continue;
            }
            if (line.rfind("level", 0) == 0) {
                long long v = atoll(trimStr(line.substr(5)).c_str());
                if (v >= 100) {
                    tms = v;
                    printf("Engine think time set to %lld ms.\n", tms);
                } else printf("Usage: level <ms>  (minimum 100, e.g. 'level 3000')\n");
                continue;
            }
            if (line == "moves") {
                MoveList ml; genMoves(pos, ml, false);
                int n = 0;
                for (int i = 0; i < ml.cnt; i++) {
                    if (!makeMove(pos, ml.moves[i])) continue;
                    unmakeMove(pos);
                    printf("%s(%s)  ", sanOf(pos, ml.moves[i]).c_str(),
                           mvToStr(ml.moves[i]).c_str());
                    if (++n % 6 == 0) printf("\n");
                }
                printf("\n%d legal moves\n", n);
                continue;
            }
            if (line == "hint") {
                int m = think(pos, 64, min(tms, 1500LL), false);
                int wsc = pos.side == WHITE ? lastScore : -lastScore;
                printf("Hint: %s  (eval %s, depth %d)\n",
                       sanOf(pos, m).c_str(), evalStr(wsc).c_str(), lastDepth);
                continue;
            }
            if (line == "eval") {
                think(pos, 64, 800, false);
                int wsc = pos.side == WHITE ? lastScore : -lastScore;
                printf("Eval (White's view): %s  (depth %d)  [+ favors White, - favors Black]\n",
                       evalStr(wsc).c_str(), lastDepth);
                continue;
            }
            if (line == "undo") {
                if (played.size() >= 2) {
                    unmakeMove(pos); unmakeMove(pos);
                    played.pop_back(); played.pop_back();
                    sanHist.pop_back(); sanHist.pop_back();
                    annot.pop_back(); annot.pop_back();
                    lastMv = 0;
                    pending = false; analyzed = false; havePre = false; preShallowBest = 0;
                    printf("Took back one move.\n");
                } else printf("Nothing to undo.\n");
                continue;
            }
            int m = parseMove(pos, line);
            if (!m) m = parseSAN(pos, line);
            if (!m) {
                printf("Invalid move: '%s' - type 'moves' to list legal moves, 'help' for commands.\n",
                       line.c_str());
                continue;
            }
            string san = sanOf(pos, m);
            // coach: gather rating inputs while the move can still be inspected
            bool cBook = false, cForced = false, cIsBest = false, cDeepOnly = false,
                 cSac = false, cForcing = false, cHaveSecond = false;
            int cSecond = 0;
            string cBestSan;
            if (coach) {
                cBook   = bookHas(played, mvToStr(m));
                cForced = (legal == 1);
                cIsBest = havePre && m == preBest;
                cDeepOnly = cIsBest && preShallowBest && m != preShallowBest;
                if (havePre) cBestSan = sanOf(pos, preBest);
                if (!cBook && cIsBest && !cForced) {
                    int pc = pos.board[mvFrom(m)];
                    cForcing = givesCheckApprox(pos, m) || isAdvancedPawnMove(pos, m) || touchesEnemyKingRing(pos, m);
                    if (pc != PAWN && pc != KING && see(pos, m) <= -150)
                        cSac = true;                    // deep-best move AND a sacrifice
                    cSecond = thinkSecond(pos, 450, m);
                    cHaveSecond = true;                 // unique/only-good-move signal
                }
            }
            makeMove(pos, m);
            played.push_back(mvToStr(m));
            sanHist.push_back(san);
            annot.push_back("");
            lastMv = m;
            analyzed = false;
            if (coach && (havePre || cBook || cForced)) {
                pending = true;
                pendIdx = sanHist.size() - 1;
                pendBook = cBook; pendForced = cForced; pendIsBest = cIsBest; pendDeepOnly = cDeepOnly;
                pendSac = cSac; pendForcing = cForcing;
                pendHaveSecond = cHaveSecond; pendSecond = cSecond;
                pendPreScore = preScore; pendBestSan = cBestSan;
            }
            havePre = false;
            printf("You: %s\n", san.c_str());
        } else {
            printf("\nEngine is thinking...\n");
            fflush(stdout);
            string bm = bookMove(played);
            int m = 0;
            bool fromBook = false;
            if (!bm.empty()) { m = parseMove(pos, bm); fromBook = (m != 0); }
            string san;
            if (fromBook) {
                if (pending) {  // book reply -> quick eval just for the rating
                    think(pos, 64, 500, false);
                    resolvePending(-lastScore);
                }
                m = verifyOrFallback(pos, m, "interactive book");
                san = sanOf(pos, m);
                printf("Engine: %s (%s)  [book]\n", san.c_str(), mvToStr(m).c_str());
            } else {
                m = think(pos, 64, tms, false);
                m = verifyOrFallback(pos, m, "interactive search");
                if (pending) resolvePending(-lastScore);
                int wsc = pos.side == WHITE ? lastScore : -lastScore;
                san = sanOf(pos, m);
                printf("Engine: %s (%s)  [depth %d, eval %s]\n",
                       san.c_str(), mvToStr(m).c_str(),
                       lastDepth, evalStr(wsc).c_str());
            }
            if (!m) { printf("Engine has no legal move.\n"); break; }
            played.push_back(mvToStr(m));
            sanHist.push_back(san);
            annot.push_back("");
            lastMv = m;
            makeMove(pos, m);
            analyzed = false;
        }
    }
}


// ---------------- built-in PGN / match report analyzer ----------------
struct AnalyzeOptions {
    string file;
    string outBase;
    string player = "SniperBishop";
    int shallowDepth = 0;      // 0 = summary-only mode
    int deepDepth = 0;         // >0 = internal engine re-analysis mode
    int maxGames = 0;          // 0 = all games
    bool allMoves = false;     // analyze both sides instead of player only
    bool pasteMode = false;    // true = read PGN/match report text from stdin
};

struct PgnGame {
    int number = 0;
    map<string,string> tag;
    vector<string> moves;
    string raw;
};

static string lowerStr(string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static bool startsWithStr(const string &s, const string &p) {
    return s.rfind(p, 0) == 0;
}
static bool endsWithStr(const string &s, const string &p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
static string baseNameNoExt(string path) {
    for (char &c : path) if (c == '\\') c = '/';
    size_t slash = path.find_last_of('/');
    string b = (slash == string::npos) ? path : path.substr(slash + 1);
    size_t dot = b.find_last_of('.');
    if (dot != string::npos) b = b.substr(0, dot);
    return b.empty() ? string("analysis") : b;
}
static string mdEscape(string s) {
    for (char &c : s) if (c == '|') c = '/';
    return s;
}
static string tagVal(const PgnGame &g, const string &k, const string &def="") {
    auto it = g.tag.find(k);
    return it == g.tag.end() ? def : it->second;
}
static int resultScoreForPlayer(const string &res, bool playerWhite) {
    if (res == "1/2-1/2") return 1; // half-point encoded as 1; win=2
    if (res == "1-0") return playerWhite ? 2 : 0;
    if (res == "0-1") return playerWhite ? 0 : 2;
    return -1;
}
static bool isResultToken(const string &t) {
    return t == "1-0" || t == "0-1" || t == "1/2-1/2" || t == "*";
}
static string cleanMoveToken(string t) {
    t = trimStr(t);
    if (t.empty()) return t;
    // Strip move-number prefixes: "12.e4" or "12...Nf6".
    size_t dot = t.find_last_of('.');
    if (dot != string::npos) {
        bool prefixDigits = true;
        for (size_t i = 0; i < dot; i++) if (!isdigit((unsigned char)t[i]) && t[i] != '.') prefixDigits = false;
        if (prefixDigits) t = t.substr(dot + 1);
    }
    while (!t.empty() && (t[0] == '.' || t[0] == ' ' || t[0] == '\t')) t.erase(t.begin());
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ',' || t.back() == ';')) t.pop_back();
    while (!t.empty() && (t.back() == '!' || t.back() == '?')) t.pop_back();
    return t;
}
static string stripPgnNoise(const string &in) {
    string out;
    int paren = 0;
    bool brace = false;
    bool semicolon = false;
    for (char ch : in) {
        if (semicolon) {
            if (ch == '\n' || ch == '\r') { semicolon = false; out += ' '; }
            continue;
        }
        if (brace) {
            if (ch == '}') brace = false;
            continue;
        }
        if (ch == ';') { semicolon = true; continue; }
        if (ch == '{') { brace = true; out += ' '; continue; }
        if (ch == '(') { paren++; out += ' '; continue; }
        if (ch == ')' && paren > 0) { paren--; out += ' '; continue; }
        if (paren > 0) continue;
        out += ch;
    }
    return out;
}
static vector<string> tokenizePgnMoves(const string &moveText) {
    string cleaned = stripPgnNoise(moveText);
    istringstream ss(cleaned);
    vector<string> toks;
    string t;
    while (ss >> t) {
        t = cleanMoveToken(t);
        if (t.empty()) continue;
        if (isResultToken(t)) continue;
        if (t[0] == '[' || t[0] == '{' || t[0] == '(') continue;
        if (t[0] == '$') continue;
        bool allDotsDigits = true;
        for (char c : t) if (!isdigit((unsigned char)c) && c != '.') allDotsDigits = false;
        if (allDotsDigits) continue;
        toks.push_back(t);
    }
    return toks;
}
static bool parseTagLine(const string &line, string &k, string &v) {
    if (line.size() < 4 || line[0] != '[') return false;
    size_t sp = line.find(' ');
    size_t q1 = line.find('"');
    size_t q2 = line.find_last_of('"');
    if (sp == string::npos || q1 == string::npos || q2 == string::npos || q2 <= q1) return false;
    k = line.substr(1, sp - 1);
    v = line.substr(q1 + 1, q2 - q1 - 1);
    return true;
}
static PgnGame parseGameChunk(const string &chunk, int num) {
    PgnGame g; g.number = num; g.raw = chunk;
    istringstream in(chunk);
    string line, moveText;
    while (getline(in, line)) {
        string t = trimStr(line);
        string k, v;
        if (parseTagLine(t, k, v)) { g.tag[k] = v; continue; }
        if (t.empty()) { moveText += ' '; continue; }
        if (startsWithStr(t, "--- Game")) continue;
        if (startsWithStr(t, "====")) continue;
        if (startsWithStr(t, "SNIPER BISHOP")) continue;
        moveText += t;
        moveText += ' ';
    }
    g.moves = tokenizePgnMoves(moveText);
    return g;
}
static vector<PgnGame> parsePgnGamesFromContent(const string &content) {
    vector<PgnGame> games;
    istringstream in(content);
    string line, chunk;
    bool active = false;
    int n = 0;
    while (getline(in, line)) {
        string t = trimStr(line);
        bool start = startsWithStr(t, "--- Game") || (startsWithStr(t, "[Event ") && active);
        if (start) {
            if (active && !chunk.empty()) {
                PgnGame g = parseGameChunk(chunk, ++n);
                if (!g.moves.empty() || !g.tag.empty()) games.push_back(g);
                chunk.clear();
            }
            active = true;
        }
        if (!active && startsWithStr(t, "[Event ")) active = true;
        if (active) { chunk += line; chunk += '\n'; }
    }
    if (active && !chunk.empty()) {
        PgnGame g = parseGameChunk(chunk, ++n);
        if (!g.moves.empty() || !g.tag.empty()) games.push_back(g);
    }
    if (games.empty() && !content.empty()) {
        PgnGame g = parseGameChunk(content, 1);
        if (!g.moves.empty() || !g.tag.empty()) games.push_back(g);
    }
    for (int i = 0; i < (int)games.size(); i++) games[i].number = i + 1;
    return games;
}
static vector<PgnGame> loadPgnGames(const string &path) {
    ifstream f(path.c_str(), ios::in | ios::binary);
    if (!f) return vector<PgnGame>();
    string content((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    return parsePgnGamesFromContent(content);
}
static string collectPastedPgn() {
    printf("Paste PGN or match_result text now.\n");
    printf("Finish with a single line containing END.\n");
    printf("----------------------------------------\n");
    string content, line;
    while (getline(cin, line)) {
        string t = trimStr(line);
        if (t == "END" || t == "DONE" || t == "EOF") break;
        content += line;
        content += '\n';
    }
    printf("----------------------------------------\n");
    return content;
}
static bool sameMoveUci(int a, int b) {
    return a && b && mvFrom(a) == mvFrom(b) && mvTo(a) == mvTo(b) && mvPromo(a) == mvPromo(b);
}
static int thinkSecondDepth(Position &pos, int depth, int exclude) {
    int svScore = lastScore, svDepth = lastDepth;
    int svExclude = rootExclude;
    rootExclude = exclude;
    think(pos, max(1, depth), -1, false);
    rootExclude = svExclude;
    int sc = lastScore;
    lastScore = svScore; lastDepth = svDepth;
    return sc;
}
static void printAnalyzeUsage() {
    printf("Usage:\n");
    printf("  analyze <file.pgn|match_result.txt> [options]\n");
    printf("  analyze                 paste PGN/match_result text, then type END\n");
    printf("  analyze paste [options]  same paste mode with options\n\n");
    printf("Options:\n");
    printf("  --player NAME       analyze this player only (default: SniperBishop)\n");
    printf("  --all               analyze both sides\n");
    printf("  --out NAME          output markdown/json basename\n");
    printf("  --max-games N       analyze only first N games\n");
    printf("  --paste             force paste mode\n");
    printf("  --deep N            enable built-in engine re-analysis to depth N\n");
    printf("  --shallow N         shallow depth for Great/Brilliant detection (default: deep-4)\n\n");
    printf("Examples:\n");
    printf("  analyze\n");
    printf("  analyze paste --deep 8 --max-games 1\n");
    printf("  analyze match_result.txt\n");
    printf("  analyze match_result.txt --deep 8 --max-games 3\n");
    printf("  analyze match_result.txt --player SniperBishop --shallow 8 --deep 14 --out analysis_full\n\n");
}
static bool readMaybeQuotedToken(istringstream &ss, string &out) {
    if (!(ss >> out)) return false;
    if (!out.empty() && out.front() == '"') {
        string more;
        while (!endsWithStr(out, "\"") && (ss >> more)) out += " " + more;
        if (!out.empty() && out.front() == '"') out.erase(out.begin());
        if (!out.empty() && out.back() == '"') out.pop_back();
    }
    return true;
}
static bool parseAnalyzeArgs(istringstream &ss, AnalyzeOptions &opt) {
    vector<string> toks;
    string tok;
    while (readMaybeQuotedToken(ss, tok)) toks.push_back(tok);

    for (size_t i = 0; i < toks.size(); i++) {
        string t = toks[i];
        if (t == "--help" || t == "-h") return false;
        if (t == "--player" && i + 1 < toks.size()) opt.player = toks[++i];
        else if (t == "--out" && i + 1 < toks.size()) opt.outBase = toks[++i];
        else if (t == "--max-games" && i + 1 < toks.size()) opt.maxGames = atoi(toks[++i].c_str());
        else if (t == "--deep" && i + 1 < toks.size()) opt.deepDepth = atoi(toks[++i].c_str());
        else if (t == "--shallow" && i + 1 < toks.size()) opt.shallowDepth = atoi(toks[++i].c_str());
        else if (t == "--all") opt.allMoves = true;
        else if (t == "--paste" || t == "paste" || t == "-") opt.pasteMode = true;
        else if (!t.empty() && t[0] != '-' && opt.file.empty()) opt.file = t;
        else {
            // Keep analyzer forgiving for pasted/interactive use.
        }
    }
    if (opt.file.empty()) opt.pasteMode = true;
    if (lowerStr(opt.file) == "paste" || opt.file == "-") { opt.pasteMode = true; opt.file.clear(); }
    if (opt.deepDepth > 0 && opt.shallowDepth <= 0) opt.shallowDepth = max(1, opt.deepDepth - 4);
    if (opt.shallowDepth > 0 && opt.deepDepth <= 0) opt.deepDepth = max(opt.shallowDepth + 4, 8);
    return true;
}
static void writeJsonMap(ofstream &js, const int *cnt) {
    js << "{";
    for (int i = 0; i < C_N; i++) {
        if (i) js << ",";
        js << "\"" << catName[i] << "\":" << cnt[i];
    }
    js << "}";
}
static void analyzeFile(const AnalyzeOptions &opt) {
    string sourceName = opt.pasteMode ? string("pasted PGN") : opt.file;
    vector<PgnGame> games;
    if (opt.pasteMode) {
        string content = collectPastedPgn();
        games = parsePgnGamesFromContent(content);
    } else {
        games = loadPgnGames(opt.file);
    }
    if (games.empty()) {
        printf("analyze: could not read games from '%s'\n", sourceName.c_str());
        return;
    }
    string outBase = opt.outBase.empty() ? (opt.pasteMode ? string("pasted_pgn_analysis_v253") : baseNameNoExt(opt.file) + "_analysis_v253") : opt.outBase;
    string mdPath = outBase + ".md";
    string jsPath = outBase + ".json";

    int maxG = opt.maxGames > 0 ? min(opt.maxGames, (int)games.size()) : (int)games.size();
    int totalGames = 0, completeGames = 0, targetGames = 0;
    int win = 0, loss = 0, draw = 0, nores = 0;
    int whiteW = 0, whiteL = 0, whiteD = 0, blackW = 0, blackL = 0, blackD = 0;
    int mateFinish = 0, promotionGames = 0, ultraLong = 0, timeForfeit = 0, illegalParse = 0;
    int catCnt[C_N] = {0};
    double accSum = 0.0; int accN = 0; long long cplSum = 0;
    vector<string> highlights;

    string playerLower = lowerStr(opt.player);
    printf("analyze: loaded %d games from %s\n", (int)games.size(), sourceName.c_str());
    if (opt.deepDepth > 0) {
        printf("analyze: built-in engine mode shallow=%d deep=%d maxGames=%d\n",
               opt.shallowDepth, opt.deepDepth, maxG);
    } else {
        printf("analyze: fast summary mode (use --deep N for Best/Great/Brilliant)\n");
    }

    for (int gi = 0; gi < maxG; gi++) {
        const PgnGame &g = games[gi];
        totalGames++;
        string white = tagVal(g, "White", "White");
        string black = tagVal(g, "Black", "Black");
        string result = tagVal(g, "Result", "*");
        string term = tagVal(g, "Termination", "");
        int plyCount = atoi(tagVal(g, "PlyCount", "0").c_str());
        bool targetWhite = lowerStr(white).find(playerLower) != string::npos;
        bool targetBlack = lowerStr(black).find(playerLower) != string::npos;
        bool gameHasTarget = opt.allMoves || targetWhite || targetBlack;
        if (gameHasTarget) targetGames++;
        int rs = -1;
        if (!opt.allMoves && (targetWhite || targetBlack)) rs = resultScoreForPlayer(result, targetWhite);
        if (rs >= 0) {
            completeGames++;
            if (rs == 2) win++; else if (rs == 1) draw++; else loss++;
            if (targetWhite) { if (rs == 2) whiteW++; else if (rs == 1) whiteD++; else whiteL++; }
            if (targetBlack) { if (rs == 2) blackW++; else if (rs == 1) blackD++; else blackL++; }
        } else if (gameHasTarget) nores++;
        if (plyCount >= 250) ultraLong++;
        if (lowerStr(term).find("time") != string::npos) timeForfeit++;
        if (g.raw.find('=') != string::npos) promotionGames++;
        if (g.raw.find('#') != string::npos || lowerStr(g.raw).find("mates") != string::npos) mateFinish++;

        if (opt.deepDepth <= 0) continue;
        if (!gameHasTarget && !opt.allMoves) continue;
        Position p;
        if (!setFen(p, START_FEN)) continue;
        vector<string> played;
        for (int mi = 0; mi < (int)g.moves.size(); mi++) {
            string tok = g.moves[mi];
            int sideBefore = p.side;
            bool analyzeThis = opt.allMoves || (targetWhite && sideBefore == WHITE) || (targetBlack && sideBefore == BLACK);
            int legalBefore = countLegal(p);
            int m = parseSAN(p, tok);
            if (!m) m = parseMove(p, tok);
            if (!m) {
                illegalParse++;
                char b[180]; snprintf(b, sizeof(b), "Game %d: could not parse move token '%s'", g.number, tok.c_str());
                if ((int)highlights.size() < 40) highlights.push_back(b);
                break;
            }
            string san = sanOf(p, m);
            if (analyzeThis) {
                Position before = p;
                bool book = bookHas(played, mvToStr(m));
                bool forced = legalBefore == 1;
                int shallowBest = opt.shallowDepth > 0 ? think(before, opt.shallowDepth, -1, false) : 0;
                int deepBest = think(before, opt.deepDepth, -1, false);
                int preSc = lastScore;
                bool isBest = sameMoveUci(m, deepBest);
                bool deepOnly = isBest && shallowBest && !sameMoveUci(shallowBest, deepBest);
                bool forcing = givesCheckApprox(before, m) || isAdvancedPawnMove(before, m) || touchesEnemyKingRing(before, m);
                bool sac = false;
                int pc = before.board[mvFrom(m)];
                if (pc != PAWN && pc != KING && see(before, m) <= -150) sac = true;
                int secondSc = 0; bool haveSecond = false;
                if (isBest && !forced && !book) { secondSc = thinkSecondDepth(before, max(1, opt.deepDepth - 1), m); haveSecond = true; }
                int postSc = preSc;
                if (makeMove(before, m)) {
                    think(before, max(1, opt.deepDepth - 1), -1, false);
                    postSc = -lastScore;
                    unmakeMove(before);
                }
                int cat = classifyMove(book, forced, isBest, deepOnly, sac, forcing, haveSecond, secondSc, preSc, postSc);
                catCnt[cat]++;
                if (cat != C_BOOK && cat != C_FORCED) {
                    int lossCp = preSc - postSc;
                    if (lossCp < 0) lossCp = 0;
                    if (lossCp > 1000) lossCp = 1000;
                    cplSum += lossCp;
                    accSum += moveAccuracy(preSc, postSc);
                    accN++;
                }
                if ((cat == C_BRILLIANT || cat == C_GREAT || cat == C_MISSED || cat == C_BLUNDER) && (int)highlights.size() < 40) {
                    char buf[260];
                    snprintf(buf, sizeof(buf), "Game %d move %d%s: %s%s = %s (deep best: %s, eval %s -> %s)",
                             g.number, mi / 2 + 1, sideBefore == BLACK ? "..." : ".",
                             san.c_str(), catSym[cat], catName[cat],
                             deepBest ? sanOf(p, deepBest).c_str() : "none",
                             evalStr(sideBefore == WHITE ? preSc : -preSc).c_str(),
                             evalStr(sideBefore == WHITE ? postSc : -postSc).c_str());
                    highlights.push_back(buf);
                }
            }
            if (!makeMove(p, m)) {
                illegalParse++;
                char b[180]; snprintf(b, sizeof(b), "Game %d: illegal move after parse '%s'", g.number, tok.c_str());
                if ((int)highlights.size() < 40) highlights.push_back(b);
                break;
            }
            played.push_back(mvToStr(m));
        }
        printf("  analyzed game %d/%d\n", gi + 1, maxG);
    }

    double scorePts = win + draw * 0.5;
    double scorePct = completeGames ? 100.0 * scorePts / completeGames : 0.0;

    ofstream md(mdPath.c_str());
    md << "# Sniper Bishop Built-in Analysis v2.53\n\n";
    md << "- Source: `" << sourceName << "`\n";
    md << "- Player: `" << (opt.allMoves ? string("ALL") : opt.player) << "`\n";
    md << "- Mode: " << (opt.deepDepth > 0 ? "engine re-analysis" : "fast summary") << "\n";
    if (opt.deepDepth > 0) md << "- Shallow/deep depth: " << opt.shallowDepth << " / " << opt.deepDepth << "\n";
    md << "\n## Result summary\n\n";
    md << "- Loaded games: " << games.size() << "\n";
    md << "- Processed games: " << maxG << "\n";
    md << "- Complete target games: " << completeGames << "\n";
    md << "- Score: " << win << "W " << loss << "L " << draw << "D  (" << scorePct << "%)\n";
    if (nores) md << "- No-result/unterminated target games: " << nores << "\n";
    md << "\n## Color split\n\n";
    md << "| Color | W | L | D |\n|---|---:|---:|---:|\n";
    md << "| White | " << whiteW << " | " << whiteL << " | " << whiteD << " |\n";
    md << "| Black | " << blackW << " | " << blackL << " | " << blackD << " |\n";
    md << "\n## Tags\n\n";
    md << "- Checkmate-ish games: " << mateFinish << "\n";
    md << "- Promotion games: " << promotionGames << "\n";
    md << "- Ultra-long games (>=250 ply): " << ultraLong << "\n";
    md << "- Time forfeit games: " << timeForfeit << "\n";
    md << "- Parse/legal issues while analyzing: " << illegalParse << "\n";
    if (opt.deepDepth > 0) {
        md << "\n## Move quality\n\n";
        md << "| Category | Count |\n|---|---:|\n";
        for (int c = 0; c < C_N; c++) if (catCnt[c]) md << "| " << catName[c] << " | " << catCnt[c] << " |\n";
        if (accN) {
            md << "\n- Average centipawn loss: " << (cplSum / accN) << "\n";
            md << "- Accuracy: " << (accSum / accN) << "%\n";
        }
        md << "\n## Critical / notable moves\n\n";
        if (highlights.empty()) md << "- No major brilliant/great/missed/blunder highlights found at this depth.\n";
        for (string h : highlights) md << "- " << mdEscape(h) << "\n";
    } else {
        md << "\n## Move quality\n\n";
        md << "Fast mode does not classify Best/Great/Brilliant. Run with `--deep N` for built-in engine re-analysis.\n";
    }
    md << "\n## Next improvement hints\n\n";
    if (blackL > blackW) md << "- Black-side stability is weaker than White-side results. Prioritize black opening safety and king defense.\n";
    if (ultraLong) md << "- Ultra-long games exist. Prioritize endgame conversion, pawn-race logic, and draw avoidance.\n";
    if (timeForfeit) md << "- Time forfeits exist. Check time management and fast endgame move selection.\n";
    if (opt.deepDepth > 0 && catCnt[C_MISSED] + catCnt[C_BLUNDER] > 0) md << "- Missed/blunder counts suggest tactical verification or LMR/futility tuning targets.\n";
    md.close();

    ofstream js(jsPath.c_str());
    js << "{\n";
    js << "  \"source\": \"" << sourceName << "\",\n";
    js << "  \"player\": \"" << (opt.allMoves ? string("ALL") : opt.player) << "\",\n";
    js << "  \"mode\": \"" << (opt.deepDepth > 0 ? "engine" : "fast") << "\",\n";
    js << "  \"processed_games\": " << maxG << ",\n";
    js << "  \"complete_games\": " << completeGames << ",\n";
    js << "  \"wins\": " << win << ", \"losses\": " << loss << ", \"draws\": " << draw << ",\n";
    js << "  \"score_percent\": " << scorePct << ",\n";
    js << "  \"white\": {\"wins\": " << whiteW << ", \"losses\": " << whiteL << ", \"draws\": " << whiteD << "},\n";
    js << "  \"black\": {\"wins\": " << blackW << ", \"losses\": " << blackL << ", \"draws\": " << blackD << "},\n";
    js << "  \"move_quality\": "; writeJsonMap(js, catCnt); js << ",\n";
    js << "  \"parse_issues\": " << illegalParse << "\n";
    js << "}\n";
    js.close();

    printf("analyze: wrote %s and %s\n", mdPath.c_str(), jsPath.c_str());
}

// ---------------- UCI ----------------
// ---------------- final legality safety net ----------------
// No matter how a move was arrived at - opening book, search, TT, whatever
// upstream logic - re-verify it against a completely fresh move generation
// on the live position, one last time, right before it's ever printed to
// the GUI. If it checks out, nothing changes. If it doesn't (whatever the
// cause - and after this session, the cause of at least some cases is
// still not fully pinned down), we fall back to any confirmed-legal move
// instead of ever handing the GUI something illegal. This can't fix a
// hidden desync bug, but it makes sure that bug can never again cost a
// game outright - worst case we play a mediocre fallback move instead of
// our real intended one, instead of forfeiting.
static bool isLegalMoveExact(Position &pos, int m) {
    if (!m) return false;
    MoveList ml;
    genMoves(pos, ml, false);
    for (int i = 0; i < ml.cnt; i++) {
        if (ml.moves[i] != m) continue;
        if (makeMove(pos, m)) { unmakeMove(pos); return true; }
        return false;
    }
    return false;
}

static bool uciRoundTripOk(Position &pos, int m) {
    if (!m) return false;
    // The exact text we send to the GUI must parse back into the exact same
    // internal move, including promotion type. This is the important part for
    // underpromotion support: f7f8r must come back as ROOK, f7f8b as BISHOP,
    // f7f8n as KNIGHT, and f7f8q as QUEEN. If this ever fails, do not output
    // the move; fall back to a move whose GUI string is known-good.
    string u = mvToStr(m);
    int parsed = parseMove(pos, u);
    return parsed == m;
}

static bool boardSanityOk(const Position &pos) {
    if (popcnt(pos.pieces[WHITE][KING]) != 1) return false;
    if (popcnt(pos.pieces[BLACK][KING]) != 1) return false;
    U64 ow = 0, ob = 0;
    for (int p = PAWN; p <= KING; p++) {
        if (ow & pos.pieces[WHITE][p]) return false;
        if (ob & pos.pieces[BLACK][p]) return false;
        ow |= pos.pieces[WHITE][p];
        ob |= pos.pieces[BLACK][p];
    }
    if (ow != pos.occ[WHITE] || ob != pos.occ[BLACK]) return false;
    if ((ow | ob) != pos.occAll) return false;
    if (ow & ob) return false;
    for (int sq = 0; sq < 64; sq++) {
        int c = colorAt(pos, sq);
        if (c < 0) {
            if (pos.board[sq] != NO_PIECE) return false;
            continue;
        }
        int p = pos.board[sq];
        if (!pieceOk(p)) return false;
        if (!(pos.pieces[c][p] & bit(sq))) return false;
    }
    return true;
}

static int chooseOrderedLegalRoundTripMove(Position &pos) {
    if (!boardSanityOk(pos)) return 0;
    MoveList ml;
    genMoves(pos, ml, false);
    scoreMoves(pos, ml, 0, 0);
    for (int i = 0; i < ml.cnt; i++) {
        pickMove(ml, i);
        int m = ml.moves[i];
        if (isLegalMoveExact(pos, m) && uciRoundTripOk(pos, m)) return m;
    }
    return 0;
}

static int verifyOrFallback(Position &pos, int bm, const char *whereFrom) {
    if (boardSanityOk(pos) && isLegalMoveExact(pos, bm) && uciRoundTripOk(pos, bm)) return bm;

    fprintf(stderr, "WARNING: %s produced a non-output-safe move (%s) for this position - "
                     "regenerating a legal round-trip-safe move instead.\n",
            whereFrom, bm ? mvToStr(bm).c_str() : "(none)");

    return chooseOrderedLegalRoundTripMove(pos);
}



// ---------------- promotion regression audit ----------------
// Covers every promotion file, not just the old hand-tested f7f8 / a2a1
// cases. This is intentionally available as a command so match-runner bugs
// can be separated from engine move encoding bugs:
//   promotest
static string auditFen(const vector<pair<int, char>> &pieces, char stm) {
    char b[64];
    for (int i = 0; i < 64; i++) b[i] = '.';
    for (auto &pc : pieces) if (sqOk(pc.first)) b[pc.first] = pc.second;

    string fen;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            char ch = b[r * 8 + f];
            if (ch == '.') empty++;
            else {
                if (empty) { fen += char('0' + empty); empty = 0; }
                fen += ch;
            }
        }
        if (empty) fen += char('0' + empty);
        if (r) fen += '/';
    }
    fen += ' ';
    fen += stm;
    fen += " - - 0 1";
    return fen;
}

static bool auditPromotionMove(const string &fen, const string &uci, int expectedPromo, bool verbose) {
    Position p;
    if (!setFen(p, fen)) {
        if (verbose) printf("FAIL fen parse: %s\n", fen.c_str());
        return false;
    }
    int us = p.side;
    int m = parseMove(p, uci);
    if (!m) {
        if (verbose) printf("FAIL parse: %-5s fen=%s\n", uci.c_str(), fen.c_str());
        return false;
    }
    if (mvPromo(m) != expectedPromo) {
        if (verbose) printf("FAIL promo bits: %-5s got=%d expected=%d\n", uci.c_str(), mvPromo(m), expectedPromo);
        return false;
    }
    if (mvToStr(m) != uci) {
        if (verbose) printf("FAIL string: %-5s rendered=%s\n", uci.c_str(), mvToStr(m).c_str());
        return false;
    }
    if (!uciRoundTripOk(p, m)) {
        if (verbose) printf("FAIL roundtrip: %-5s\n", uci.c_str());
        return false;
    }
    int to = mvTo(m);
    if (!makeMove(p, m)) {
        if (verbose) printf("FAIL makeMove: %-5s fen=%s\n", uci.c_str(), fen.c_str());
        return false;
    }
    if (!hasPiece(p, us, expectedPromo, to)) {
        if (verbose) printf("FAIL board piece: %-5s target=%s expectedPromo=%d\n", uci.c_str(), sqName(to).c_str(), expectedPromo);
        return false;
    }
    unmakeMove(p);
    return true;
}

static bool runPromotionAudit(bool verbose = true) {
    const char suffix[4] = {'q','r','b','n'};
    const int promo[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
    int tests = 0, fail = 0;

    auto check = [&](const string &fen, const string &uci, int p) {
        tests++;
        if (!auditPromotionMove(fen, uci, p, verbose)) fail++;
    };

    for (int f = 0; f < 8; f++) {
        // White straight promotions: a7a8..h7h8, all q/r/b/n.
        int fromW = 6 * 8 + f, toW = 7 * 8 + f;
        string fenW = auditFen({{4, 'K'}, {36, 'k'}, {fromW, 'P'}}, 'w');
        for (int i = 0; i < 4; i++) check(fenW, sqName(fromW) + sqName(toW) + suffix[i], promo[i]);

        // Black straight promotions: a2a1..h2h1, all q/r/b/n.
        int fromB = 1 * 8 + f, toB = f;
        string fenB = auditFen({{28, 'K'}, {60, 'k'}, {fromB, 'p'}}, 'b');
        for (int i = 0; i < 4; i++) check(fenB, sqName(fromB) + sqName(toB) + suffix[i], promo[i]);

        // Capture promotions on both diagonals where they exist.
        for (int df = -1; df <= 1; df += 2) {
            int tf = f + df;
            if (tf < 0 || tf > 7) continue;

            int capToW = 7 * 8 + tf;
            string fenCW = auditFen({{4, 'K'}, {36, 'k'}, {fromW, 'P'}, {capToW, 'r'}}, 'w');
            for (int i = 0; i < 4; i++) check(fenCW, sqName(fromW) + sqName(capToW) + suffix[i], promo[i]);

            int capToB = tf;
            string fenCB = auditFen({{28, 'K'}, {60, 'k'}, {fromB, 'p'}, {capToB, 'R'}}, 'b');
            for (int i = 0; i < 4; i++) check(fenCB, sqName(fromB) + sqName(capToB) + suffix[i], promo[i]);
        }
    }

    if (verbose) printf("promotion audit: %d tests, %d failed\n", tests, fail);
    return fail == 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    try {
        if (argc > 0 && argv && argv[0])
            FirstNet::exeDirectory = filesystem::absolute(filesystem::path(argv[0])).parent_path();
    } catch (...) { FirstNet::exeDirectory.clear(); }
    initConsole();
    initLeapers();
    initMagics(true);
    initMagics(false);
    initZobrist();
    initCastleMask();
    initEvalMasks();
    initLmr();
    ttInit(64);
    memset(historyTbl, 0, sizeof(historyTbl));
    FirstNet::load(FirstNet::evalFile, false);
    printf("\n");
    printf("  =====================================\n");
    printf("     SNIPER BISHOP  -  Chess Engine 2.61 FIRST_NET HYBRID TUNED DEV\n");
    printf("  =====================================\n");
    printf("  play : play a game in this terminal\n");
    printf("  uci  : UCI mode (for chess GUIs)\n");
    printf("  analyze        - paste PGN/match_result text, then type END\n");
            printf("  analyze <file> - built-in PGN/match_result analyzer\n");
    printf("  help : list all commands\n\n");

    Position pos;
    setFen(pos, START_FEN);
    vector<string> played;
    bool fromStart = true;
    bool positionOk = true;
    string positionError;

    string line;
    while (getline(cin, line)) {
        istringstream ss(line);
        string cmd;
        ss >> cmd;
        if (cmd == "uci") {
            printf("id name Sniper Bishop 2.61 FIRST_NET Hybrid Tuned\n");
            printf("id author Blucy1004\n");
            printf("option name Hash type spin default 64 min 1 max 4096\n");
            printf("option name Build type string default V261_HybridGuard_FIRST_NET_v3_20260713\n");
            printf("option name Underpromotion type check default true\n");
            printf("option name UseNNUE type check default true\n");
            printf("option name EvalFile type string default firstnet_v3.snnue\n");
            printf("option name NNUEBlend type spin default 35 min 0 max 100\n");
            printf("option name HybridGuard type check default true\n");
            if (FirstNet::net.loaded)
                printf("info string NNUE ready: %s\n", FirstNet::net.loadedPath.c_str());
            else if (!FirstNet::net.lastError.empty())
                printf("info string NNUE unavailable: %s\n", FirstNet::net.lastError.c_str());
            printf("uciok\n");
        } else if (cmd == "isready") {
            if (FirstNet::useNNUE && !FirstNet::net.loaded)
                FirstNet::load(FirstNet::evalFile, true);
            printf("readyok\n");
        } else if (cmd == "setoption") {
            string tok, name, value;
            while (ss >> tok) {
                if (tok == "name") ss >> name;
                else if (tok == "value") ss >> value;
            }
            if (name == "Hash" && !value.empty()) {
                configuredHashMb = clamp(atoi(value.c_str()), 1, 4096);
                ttInit(configuredHashMb);
            } else if (name == "UseNNUE" && !value.empty()) {
                FirstNet::useNNUE = (value == "true" || value == "1" || value == "on");
                if (FirstNet::useNNUE && !FirstNet::net.loaded)
                    FirstNet::load(FirstNet::evalFile, true);
                ttInit(configuredHashMb);
            } else if (name == "EvalFile" && !value.empty()) {
                FirstNet::evalFile = value;
                FirstNet::load(FirstNet::evalFile, true);
                ttInit(configuredHashMb);
            } else if (name == "NNUEBlend" && !value.empty()) {
                FirstNet::blendPercent = clamp(atoi(value.c_str()), 0, 100);
                ttInit(configuredHashMb);
            } else if (name == "HybridGuard" && !value.empty()) {
                FirstNet::hybridGuard = (value == "true" || value == "1" || value == "on");
                ttInit(configuredHashMb);
            }
        } else if (cmd == "ucinewgame") {
            ttInit(configuredHashMb);
            memset(historyTbl, 0, sizeof(historyTbl));
            setFen(pos, START_FEN);
            played.clear(); fromStart = true;
            positionOk = true; positionError.clear();
        } else if (cmd == "position") {
            string tok; ss >> tok;
            played.clear();
            positionOk = true; positionError.clear();
            if (tok == "startpos") {
                if (!setFen(pos, START_FEN)) { positionOk = false; positionError = "bad startpos"; }
                fromStart = true;
                ss >> tok;  // maybe "moves" / "san"
            } else if (tok == "fen") {
                string fen, part; int cnt = 0;
                while (ss >> part) {
                    if (part == "moves" || part == "san") { tok = part; break; }
                    fen += (cnt++ ? " " : "") + part;
                }
                if (!setFen(pos, fen)) { positionOk = false; positionError = "bad fen"; }
                fromStart = false;
            }
            if (positionOk && (tok == "moves" || tok == "san")) {
                bool sanMode = (tok == "san");
                string m;
                while (ss >> m) {
                    if (m.find('.') != string::npos) continue;   // skip "1." numbers
                    if (m == "1-0" || m == "0-1" || m == "1/2-1/2" || m == "*") break;
                    int mv = sanMode ? parseSAN(pos, m) : parseMove(pos, m);
                    if (!mv) mv = sanMode ? parseMove(pos, m) : parseSAN(pos, m);
                    if (!mv) {
                        positionOk = false;
                        positionError = string("bad move in position list: ") + m;
                        printf("info string bad move '%s'\n", m.c_str());
                        break;
                    }
                    if (!makeMove(pos, mv)) {
                        positionOk = false;
                        positionError = string("illegal move in position list: ") + m;
                        printf("info string illegal move in position list '%s'\n", m.c_str());
                        break;
                    }
                    if (!boardSanityOk(pos)) {
                        positionOk = false;
                        positionError = string("board sanity failed after move: ") + m;
                        printf("info string board sanity failed after move '%s'\n", m.c_str());
                        break;
                    }
                    played.push_back(mvToStr(mv));
                }
            }

        } else if (cmd == "go") {
            string tok;
            long long wtime = -1, btime = -1, winc = 0, binc = 0, movetime = -1;
            int depth = 64, movestogo = 30;
            while (ss >> tok) {
                if (tok == "wtime") ss >> wtime;
                else if (tok == "btime") ss >> btime;
                else if (tok == "winc") ss >> winc;
                else if (tok == "binc") ss >> binc;
                else if (tok == "movetime") ss >> movetime;
                else if (tok == "depth") ss >> depth;
                else if (tok == "movestogo") ss >> movestogo;
                else if (tok == "infinite") movetime = -2;
            }
            if (!positionOk || !boardSanityOk(pos)) {
                printf("info string POSITION_DESYNC %s\n", positionError.empty() ? "board sanity failed" : positionError.c_str());
                int safe = chooseOrderedLegalRoundTripMove(pos);
                printf("bestmove %s\n", safe ? mvToStr(safe).c_str() : "0000");
                continue;
            }
            // opening book
            if (fromStart) {
                string bm = bookMove(played);
                int bmMove = bm.empty() ? 0 : parseMove(pos, bm);
                if (bmMove) {
                    bmMove = verifyOrFallback(pos, bmMove, "opening book");
                    printf("bestmove %s\n", mvToStr(bmMove).c_str());
                    continue;
                }
            }
            long long tms = -1;
            if (movetime > 0) tms = movetime;
            else if (movetime == -2) tms = -1;
            else {
                long long myTime = pos.side == WHITE ? wtime : btime;
                long long myInc  = pos.side == WHITE ? winc : binc;
                if (myTime >= 0) {
                    tms = myTime / max(2, movestogo) + myInc / 2;
                    if (tms > myTime - 100) tms = max(50LL, myTime - 100);
                }
            }
            int bm = think(pos, depth, tms, true);
            bm = verifyOrFallback(pos, bm, "search");
            printf("bestmove %s\n", bm ? mvToStr(bm).c_str() : "0000");
        } else if (cmd == "perft") {
            int d = 5; ss >> d;
            long long start = nowMs();
            U64 n = perft(pos, d);
            long long el = nowMs() - start;
            printf("perft(%d) = %llu  (%lld ms, %lld knps)\n", d,
                   (unsigned long long)n, el, el > 0 ? (long long)(n / el) : 0);
        } else if (cmd == "divide") {
            int d = 1; ss >> d;
            MoveList ml; genMoves(pos, ml, false);
            U64 total = 0;
            for (int i = 0; i < ml.cnt; i++) {
                if (!makeMove(pos, ml.moves[i])) continue;
                U64 n = perft(pos, d - 1);
                unmakeMove(pos);
                printf("%s: %llu\n", mvToStr(ml.moves[i]).c_str(), (unsigned long long)n);
                total += n;
            }
            printf("total: %llu\n", (unsigned long long)total);
        } else if (cmd == "promotest") {
            runPromotionAudit(true);
        } else if (cmd == "d") {
            printBoard(pos);
            printf("side: %s  castle: %d  ep: %d  key: %016llx\n",
                   pos.side == WHITE ? "white" : "black", pos.castle, pos.ep,
                   (unsigned long long)pos.key);
            printf("fen: %s\n\n", posToFen(pos, 1).c_str());
        } else if (cmd == "eval") {
            int classical = evaluateClassical(pos);
            if (FirstNet::net.loaded) {
                int whiteCp = FirstNet::whiteCp(pos);
                int scratchWhiteCp = FirstNet::whiteCpScratch(pos);
                int nnueStm = pos.side == WHITE ? whiteCp : -whiteCp;
                printf("static eval selected: %d cp (side to move)\n", evaluate(pos));
                printf("  Classical: %d cp  FIRST_NET: %d cp STM / %d cp WhitePOV (incremental)\n",
                       classical, nnueStm, whiteCp);
                printf("  Scratch oracle: %d cp WhitePOV  diff=%d  Blend: %d%%\n",
                       scratchWhiteCp, whiteCp - scratchWhiteCp, FirstNet::blendPercent);
            } else {
                printf("static eval selected: %d cp (Classical fallback; NNUE not loaded)\n", classical);
            }
        } else if (cmd == "nnuecheck") {
            bool loaded = FirstNet::net.loaded || FirstNet::load(FirstNet::evalFile, true);
            bool exact = loaded && FirstNet::exactPosition(pos, true);
            printf("NNUE status: %s  exact=%s  UseNNUE=%s  Blend=%d  EvalFile=%s\n",
                   loaded ? "loaded" : "failed",
                   exact ? "PASS" : "FAIL",
                   FirstNet::useNNUE ? "true" : "false",
                   FirstNet::blendPercent,
                   FirstNet::evalFile.c_str());
        } else if (cmd == "nnuewhitebox") {
            bool ok = runNNUEWhitebox(true);
            printf("NNUE WHITEBOX VERDICT: %s\n", ok ? "PASS" : "FAIL");
        } else if (cmd == "nnueverify") {
            int target = 100000;
            ss >> target;
            bool whitebox = runNNUEWhitebox(true);
            bool random = whitebox && runNNUERandomVerify(target, true);
            printf("NNUE VERIFY VERDICT: %s\n", (whitebox && random) ? "PASS" : "FAIL");
        } else if (cmd == "nnuestats") {
            FirstNet::printTelemetry();
        } else if (cmd == "nnueresetstats") {
            FirstNet::resetTelemetry();
            printf("NNUE telemetry reset\n");
        } else if (cmd == "play") {
            playMode(pos);
            setFen(pos, START_FEN);
            played.clear(); fromStart = true;
            positionOk = true; positionError.clear();
        } else if (cmd == "analyze") {
            AnalyzeOptions opt;
            if (!parseAnalyzeArgs(ss, opt)) printAnalyzeUsage();
            else analyzeFile(opt);
            setFen(pos, START_FEN);
            played.clear(); fromStart = true;
            positionOk = true; positionError.clear();
        } else if (cmd == "quit") {
            break;
        } else if (cmd == "help") {
            printf("commands:\n");
            printf("  play          - interactive game vs the engine (recommended)\n");
            printf("  uci           - switch to UCI protocol (for chess GUIs like Arena/CuteChess)\n");
            printf("  analyze FILE  - built-in PGN/match_result analyzer\n");
            printf("                  examples: analyze\n");
            printf("                            analyze paste --deep 8 --max-games 1\n");
            printf("                            analyze match.txt --deep 8 --max-games 3\n");
            printf("  position ...  - set up a position (UCI syntax)\n");
            printf("  go ...        - start a search (UCI syntax)\n");
            printf("  d             - display current board + FEN\n");
            printf("  eval          - static evaluation of current position\n");
            printf("  nnuecheck     - compare incremental accumulator with scratch oracle\n");
            printf("  nnuewhitebox  - targeted move/make/unmake exact tests\n");
            printf("  nnueverify N  - whitebox + N random checked plies (default 100000)\n");
            printf("  nnuestats     - print incremental NNUE telemetry\n");
            printf("  perft N       - move generation test to depth N\n");
            printf("  divide N      - perft split by first move\n");
            printf("  promotest     - promotion legality audit\n");
            printf("  quit          - exit\n");
        }
    }
    return 0;
}

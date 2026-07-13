#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
Horizontal Rook v1.10.0 (Paired + Seeded + Provenance)
SniperBishop local match runner + verdict tool.

Tool package stamp: v1.10.0 PairedSeeded 20260711. Ctrl+C partial reports and unfinished games are handled safely. Fresh-run guard prevents old PGNs from being mixed into new reports. Fixed-color modes run one fresh pairing per game, so blacktest is genuinely black-only.

This version is matched to the user's flat SniperBishop folder:
  SniperBishop/
    Eunshinbishop.exe
    external/cutechess-cli.exe
    external/stockfish.exe
    .dll/               # preserved, never modified; added to PATH only while running
    results/            # all game reports, PGNs, CSVs go here
    tools/

Quick examples from inside SniperBishop:
  python .\tools\horizontal_rook.py run --limit-elo 2800 --games 30 --tc 10+0.1 --label sf2800_30g
  python .\tools\horizontal_rook.py fishhunter --games 30 --tc 10+0.1 --label full_30g
  python .\tools\horizontal_rook.py report .\results\sf2800_30g.txt --opponent-elo 2800
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import math
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

TAG_RE = re.compile(r'\[(\w+)\s+"([^"]*)"\]')
GAME_BLOCK_RE = re.compile(r'(?:--- Game \d+ ---\s*)?(\[Event.*?)(?=\n--- Game \d+ ---|\n\[Event |\Z)', re.S)
FINISHED_RE = re.compile(r"^Finished game (\d+)")
PROTOCOL_LINE_RE = re.compile(r"^\d+\s+[<>]")


def project_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_path(value: str | None, root: Path) -> Optional[Path]:
    if not value:
        return None
    p = Path(value)
    if p.is_absolute():
        return p
    return root / p


def sha256_file(path: Optional[Path]) -> str:
    if path is None or not path.is_file():
        return "MISSING"
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


@dataclass
class Game:
    index: int
    text: str
    tags: Dict[str, str]

    @property
    def white(self) -> str: return self.tags.get("White", "?")
    @property
    def black(self) -> str: return self.tags.get("Black", "?")
    @property
    def result(self) -> str: return self.tags.get("Result", "*")
    @property
    def opening(self) -> str:
        opening = self.tags.get("Opening", "?")
        variation = self.tags.get("Variation", "")
        return opening if not variation else f"{opening} / {variation}"
    @property
    def ply(self) -> int:
        try: return int(self.tags.get("PlyCount", "0"))
        except ValueError: return 0
    @property
    def termination(self) -> str: return self.tags.get("Termination", "")

    def has_bad_marker(self) -> bool:
        lower = self.text.lower()
        return any(x in lower for x in ["illegal", "position_desync", "desync", "bestmove 0000", " 0000"])

    def is_time_forfeit(self) -> bool:
        lower = (self.termination + "\n" + self.text).lower()
        return "time forfeit" in lower or "loses on time" in lower or "forfeit" in lower

    def winner(self) -> Optional[str]:
        if self.result == "1-0": return self.white
        if self.result == "0-1": return self.black
        return None

    def player_color(self, player: str) -> Optional[str]:
        if name_match(player, self.white): return "White"
        if name_match(player, self.black): return "Black"
        return None

    def is_finished(self) -> bool:
        return self.result in {"1-0", "0-1", "1/2-1/2"}

    def score_for(self, player: str) -> Optional[float]:
        if not self.player_color(player) or not self.is_finished(): return None
        if self.result == "1/2-1/2": return 0.5
        w = self.winner()
        return 1.0 if w and name_match(player, w) else 0.0


def name_match(needle: str, haystack: str) -> bool:
    if not needle or not haystack:
        return False
    a = needle.lower().replace(" ", "").replace("-", "")
    b = haystack.lower().replace(" ", "").replace("-", "")
    return a in b or b in a


def parse_games(path: Path) -> List[Game]:
    text = path.read_text(encoding="utf-8", errors="replace")
    blocks = [b.strip() for b in GAME_BLOCK_RE.findall(text) if b.strip()]
    if not blocks and "[Event" in text:
        parts = re.split(r'(?=\[Event )', text)
        blocks = [p.strip() for p in parts if p.strip().startswith("[Event")]
    games: List[Game] = []
    for i, block in enumerate(blocks, 1):
        tags = dict(TAG_RE.findall(block))
        if tags:
            games.append(Game(i, block, tags))
    return games


def stats_for(games: List[Game], player: str) -> Dict[str, object]:
    played = [g for g in games if g.player_color(player) and g.is_finished()]
    unfinished = [g for g in games if g.player_color(player) and not g.is_finished()]
    wins = losses = draws = pure_wins = time_wins = time_losses = time_draws = bad_games = 0
    color = {"White": {"W": 0, "L": 0, "D": 0}, "Black": {"W": 0, "L": 0, "D": 0}}
    openings: Dict[str, Dict[str, int]] = {}
    longest = sorted(played, key=lambda g: g.ply, reverse=True)[:5]
    for g in played:
        sc = g.score_for(player)
        c = g.player_color(player) or "?"
        openings.setdefault(g.opening, {"W": 0, "L": 0, "D": 0, "G": 0})["G"] += 1
        if g.has_bad_marker(): bad_games += 1
        timed = g.is_time_forfeit()
        if sc == 1.0:
            wins += 1; color[c]["W"] += 1; openings[g.opening]["W"] += 1
            if timed: time_wins += 1
            elif not g.has_bad_marker(): pure_wins += 1
        elif sc == 0.5:
            draws += 1; color[c]["D"] += 1; openings[g.opening]["D"] += 1
            if timed: time_draws += 1
        else:
            losses += 1; color[c]["L"] += 1; openings[g.opening]["L"] += 1
            if timed: time_losses += 1
    total = wins + losses + draws
    score = wins + 0.5 * draws
    pct = 100.0 * score / total if total else 0.0
    return {"played": total, "unfinished": len(unfinished), "wins": wins, "losses": losses, "draws": draws, "score": score, "pct": pct,
            "pure_wins": pure_wins, "time_wins": time_wins, "time_losses": time_losses,
            "time_draws": time_draws, "time_forfeits": time_wins + time_losses + time_draws,
            "bad_games": bad_games,
            "color": color, "openings": openings, "longest": longest}


def elo_delta(score_rate: float) -> Optional[float]:
    score_rate = min(0.99, max(0.01, score_rate))
    try:
        return -400.0 * math.log10(1.0 / score_rate - 1.0)
    except (ValueError, ZeroDivisionError):
        return None


def report_text(games: List[Game], player: str, opponent_elo: Optional[int] = None, title: str = "Horizontal Rook Report") -> str:
    s = stats_for(games, player)
    lines: List[str] = []
    lines.append("=" * 64)
    lines.append(title)
    lines.append("=" * 64)
    lines.append(f"Player: {player}")
    lines.append(f"Finished games counted: {s['played']}")
    if s.get("unfinished"):
        lines.append(f"Unfinished/ignored games: {s['unfinished']}")
    lines.append(f"Result: {s['wins']}W {s['losses']}L {s['draws']}D  ({s['pct']:.1f}%)")
    lines.append(
        f"Pure chess wins: {s['pure_wins']}  |  Time forfeits W/L/D: "
        f"{s['time_wins']}/{s['time_losses']}/{s['time_draws']}  |  "
        f"Bad-marker games: {s['bad_games']}"
    )
    if s["played"]:
        d = elo_delta(float(s["score"]) / float(s["played"]))
        if d is not None:
            lines.append(f"Score Elo delta: {d:+.0f}")
            if opponent_elo is not None:
                lines.append(f"Performance estimate vs {opponent_elo}: {opponent_elo + d:.0f}")
    lines.append("")
    lines.append("Color split")
    color_pcts = {}
    for c in ["White", "Black"]:
        cs = s["color"][c]
        total = cs["W"] + cs["L"] + cs["D"]
        pct = 100 * (cs["W"] + 0.5 * cs["D"]) / total if total else 0
        color_pcts[c] = pct
        lines.append(f"  {c:<5}: {cs['W']}W {cs['L']}L {cs['D']}D  ({pct:.1f}%)")
    white_total = sum(s["color"]["White"].values())
    black_total = sum(s["color"]["Black"].values())
    if white_total >= 4 and black_total >= 4:
        gap = color_pcts["White"] - color_pcts["Black"]
        if gap >= 35:
            lines.append(f"  ALERT: color gap {gap:.1f}%p. BlackCounter patch/test recommended.")
        elif gap <= -35:
            lines.append(f"  ALERT: reverse color gap {-gap:.1f}%p. White repertoire check recommended.")
    lines.append("")
    lines.append("Opening table (top by games)")
    opening_rows = sorted(s["openings"].items(), key=lambda kv: (-kv[1]["G"], kv[0]))[:12]
    for op, os_ in opening_rows:
        total = os_["G"]
        pct = 100 * (os_["W"] + 0.5 * os_["D"]) / total if total else 0
        lines.append(f"  {op[:45]:<45} {os_['W']}W {os_['L']}L {os_['D']}D ({pct:.1f}%)")
    lines.append("")
    lines.append("Longest games")
    for g in s["longest"]:
        lines.append(f"  Game {g.index}: {g.ply} ply, {g.result}, {g.opening}")
    lines.append("")
    lines.append("Verdict")
    if s["bad_games"]:
        lines.append("  FAIL/CAUTION: illegal/desync/0000 marker appeared. Fix legality/stability first.")
    elif s["played"] >= 30 and s["pct"] >= 53:
        lines.append("  PASS candidate: score is above 53% with no bad markers.")
    elif s["played"] >= 30 and s["pct"] < 47:
        lines.append("  FAIL candidate: score is below 47%; likely weaker or bad matchup.")
    else:
        lines.append("  NEED MORE GAMES: sample is too small or close to call.")
    return "\n".join(lines) + "\n"


def write_csv(games: List[Game], path: Path, player: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["game", "white", "black", "result", "player_color", "player_score", "ply", "opening", "termination", "time_forfeit", "bad_marker"])
        for g in games:
            score = g.score_for(player)
            w.writerow([g.index, g.white, g.black, g.result, g.player_color(player) or "", "" if score is None else score, g.ply, g.opening, g.termination, int(g.is_time_forfeit()), int(g.has_bad_marker())])


def game_blocks_for_report(games: List[Game]) -> str:
    parts: List[str] = []
    for g in games:
        parts.append(f"--- Game {g.index} ---\n{g.text.strip()}\n")
    return "\n".join(parts)


def build_cutechess_cmd(args, pgn_path: Path, root: Path, rounds_override: Optional[int] = None) -> List[str]:
    cutechess_path = resolve_path(args.cutechess, root)
    engine_path = resolve_path(args.engine, root)
    opponent_path = resolve_path(args.opponent, root)
    cmd = [str(cutechess_path)]
    opponent_options = args.opponent_options or ""
    if args.limit_elo and not opponent_options:
        opponent_options = f"option.UCI_LimitStrength=true option.UCI_Elo={args.limit_elo}"

    color = getattr(args, "color", "both") or "both"
    color = color.lower()

    def add_player_engine():
        cmd.extend(["-engine", f"cmd={engine_path}", f"name={args.player_name}"])
        if args.engine_options:
            cmd.extend(shlex.split(args.engine_options))

    def add_opponent_engine():
        cmd.extend(["-engine", f"cmd={opponent_path}", f"name={args.opponent_name}"])
        if opponent_options:
            cmd.extend(shlex.split(opponent_options))

    # CuteChess uses the first engine as White in a single pairing.  CuteChess
    # alternates colors across tournament rounds, so fixed-color modes are run
    # as repeated one-game pairings by run_match(); engine order here sets the
    # requested color for each of those one-game processes.
    if color == "black":
        add_opponent_engine(); add_player_engine()
    else:
        add_player_engine(); add_opponent_engine()

    cmd += ["-each", "proto=uci", f"tc={args.tc}"]
    if color == "both":
        if args.games % 2:
            raise ValueError("paired color mode requires an even --games value")
        cmd += ["-games", "2", "-rounds", str(args.games // 2), "-repeat"]
    else:
        rounds = args.games if rounds_override is None else rounds_override
        cmd += ["-rounds", str(rounds)]
    cmd += ["-recover", "-pgnout", str(pgn_path)]
    seed = getattr(args, "seed", 1)
    if seed is not None:
        cmd += ["-srand", str(seed)]
    if args.concurrency > 1:
        cmd += ["-concurrency", str(args.concurrency)]
    if args.book:
        book_path = resolve_path(args.book, root)
        book_format = getattr(args, "book_format", None)
        if not book_format:
            book_format = "epd" if book_path.suffix.lower() == ".epd" else "pgn"
        cmd += [
            "-openings",
            f"file={book_path}",
            f"format={book_format}",
            f"order={getattr(args, 'book_order', 'sequential')}",
            "policy=round",
        ]
        book_plies = getattr(args, "book_plies", None)
        if book_plies is not None:
            cmd.append(f"plies={book_plies}")
    if getattr(args, "debug_log", False):
        # CuteChess 1.5.1 documents this as a flag, but its Windows parser
        # requires the explicit enum value "all".
        cmd += ["-debug", "all"]
    return cmd


def make_env(root: Path) -> Dict[str, str]:
    env = os.environ.copy()
    # .dll is intentionally not moved or modified. It is only added to PATH for CuteChess Qt DLL loading.
    dll_dir = root / ".dll"
    ext_dir = root / "external"
    prepend = [str(p) for p in [dll_dir, ext_dir] if p.exists()]
    env["PATH"] = os.pathsep.join(prepend + [env.get("PATH", "")])
    return env


def clean_label_name(label: str) -> str:
    label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label.strip())
    return label.strip("._-") or "match"


def timestamp_label(base: str) -> str:
    return f"{clean_label_name(base)}_{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}"


def archive_existing_outputs(paths: List[Path], archive_dir: Path, label: str) -> List[str]:
    moved: List[str] = []
    archive_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    for path in paths:
        if not path.exists():
            continue
        dest = archive_dir / f"{path.stem}__archived_{stamp}{path.suffix}"
        n = 2
        while dest.exists():
            dest = archive_dir / f"{path.stem}__archived_{stamp}_{n}{path.suffix}"
            n += 1
        path.replace(dest)
        moved.append(f"{path.name} -> archive/{dest.name}")
    return moved


def remove_if_exists(paths: List[Path]) -> None:
    for path in paths:
        try:
            if path.exists():
                path.unlink()
        except OSError:
            pass


def _run_cutechess_process(cmd: List[str], root: Path, progress_total: int,
                           progress_offset: int = 0,
                           log_path: Optional[Path] = None) -> Tuple[bool, int]:
    """Run one CuteChess process and return (interrupted, exit code)."""
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, bufsize=1, env=make_env(root))
    interrupted = False
    log_handle = None
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_handle = log_path.open("a", encoding="utf-8", newline="\n")
    try:
        while True:
            line = proc.stdout.readline() if proc.stdout else ""
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue
            line = line.rstrip("\n")
            if log_handle is not None:
                log_handle.write(line + "\n")
                log_handle.flush()
            m = FINISHED_RE.match(line)
            if m:
                done = progress_offset + int(m.group(1))
                print(f"[progress] game {done}/{progress_total}")
            elif line.strip() and not (log_path is not None and PROTOCOL_LINE_RE.match(line)):
                print("[cutechess]", line)
    except KeyboardInterrupt:
        interrupted = True
        print("\n[Horizontal Rook] stop requested; trying clean stop...")
        try:
            if os.name != "nt": proc.send_signal(signal.SIGINT)
            else: proc.terminate()
        except OSError:
            pass
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
    finally:
        try:
            proc.wait(timeout=3)
        except Exception:
            pass
        if log_handle is not None:
            log_handle.close()
    return interrupted, int(proc.returncode or 0)


def _append_pgn(src: Path, dest: Path) -> None:
    if not src.exists():
        return
    text = src.read_text(encoding="utf-8", errors="replace").strip()
    if text:
        with dest.open("a", encoding="utf-8", newline="\n") as f:
            if dest.stat().st_size:
                f.write("\n\n")
            f.write(text)
            f.write("\n")
    try:
        src.unlink()
    except OSError:
        pass


def run_match(args) -> None:
    root = project_root()
    out_dir = resolve_path(args.out_dir, root) or (root / "results")
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    label = args.label or f"match_result_{stamp}"
    if not label.startswith("match_result_") and args.prefix_match_result:
        label = f"match_result_{label}"
    label = clean_label_name(label)
    pgn_path = out_dir / f"{label}_live.pgn"
    report_path = out_dir / f"{label}.txt"
    csv_path = out_dir / f"{label}.csv"
    protocol_path = out_dir / f"{label}_protocol.log" if getattr(args, "debug_log", False) else None

    if not getattr(args, "resume", False):
        guarded_paths = [pgn_path, report_path, csv_path]
        if protocol_path is not None:
            guarded_paths.append(protocol_path)
        moved = archive_existing_outputs(guarded_paths, out_dir / "archive", label)
        if moved:
            print("[Horizontal Rook] fresh-run guard: archived existing files for this label")
            for item in moved:
                print("  ", item)
        remove_if_exists(guarded_paths)

    if args.limit_elo and args.opponent_name == "Stockfish":
        args.opponent_name = f"Stockfish-{args.limit_elo}"
    if args.limit_elo and args.opponent_elo is None:
        args.opponent_elo = args.limit_elo

    color = (getattr(args, "color", "both") or "both").lower()
    print("[Horizontal Rook] project root:", root)
    print("[Horizontal Rook] output:", report_path)
    print("[Horizontal Rook] Ctrl+C = stop and write report from current PGN if possible")

    stopped = False
    exit_codes: List[int] = []
    first_command: Optional[List[str]] = None
    if color in {"white", "black"}:
        if getattr(args, "concurrency", 1) > 1:
            print("[Horizontal Rook] fixed-color mode uses concurrency=1 to preserve exact colors.")
        print(f"[Horizontal Rook] TrueColor mode: SniperBishop plays {color.upper()} in every game")
        for game_no in range(1, args.games + 1):
            tmp_pgn = out_dir / f".{label}_fixed_{game_no:04d}.pgn"
            remove_if_exists([tmp_pgn])
            cmd = build_cutechess_cmd(args, tmp_pgn, root, rounds_override=1)
            if first_command is None:
                first_command = list(cmd)
            if game_no == 1:
                print("[Horizontal Rook] running:", " ".join(f'"{c}"' if " " in c else c for c in cmd))
            interrupted, exit_code = _run_cutechess_process(
                cmd, root, args.games, game_no - 1, protocol_path
            )
            exit_codes.append(exit_code)
            _append_pgn(tmp_pgn, pgn_path)
            parsed_now = len(parse_games(pgn_path)) if pgn_path.exists() else 0
            print(f"[fixed-color] completed/recorded {parsed_now}/{args.games}")
            if interrupted:
                stopped = True
                break
    else:
        cmd = build_cutechess_cmd(args, pgn_path, root)
        first_command = list(cmd)
        print("[Horizontal Rook] running:", " ".join(f'"{c}"' if " " in c else c for c in cmd))
        stopped, exit_code = _run_cutechess_process(
            cmd, root, args.games, log_path=protocol_path
        )
        exit_codes.append(exit_code)

    games = parse_games(pgn_path) if pgn_path.exists() else []
    txt = report_text(games, args.player_name, args.opponent_elo,
                      title=f"Sniper Bishop automated match report (Horizontal Rook) - {label}")
    parsed_count = len(games)
    mismatch = parsed_count > args.games or (not stopped and parsed_count != args.games)
    engine_path = resolve_path(args.engine, root)
    opponent_path = resolve_path(args.opponent, root)
    command_text = " ".join(first_command or [])
    header = [
        "=" * 64,
        "Sniper Bishop automated match report",
        "Generated by: Horizontal Rook v1.10.0 (Paired + Seeded + Provenance)",
        "=" * 64,
        f"Started/label:   {label}",
        f"Time control:    {args.tc}",
        f"Requested color: {color}",
        f"Requested games: {args.games}",
        f"Parsed games:    {parsed_count}",
        f"Stopped early:   {'yes' if stopped else 'no'}",
        f"Live PGN:        {pgn_path.name}",
        f"Protocol log:    {protocol_path.name if protocol_path else 'not requested'}",
        f"Random seed:     {getattr(args, 'seed', 1)}",
        f"Engine SHA-256:  {sha256_file(engine_path)}",
        f"Opponent SHA-256: {sha256_file(opponent_path)}",
        f"CuteChess exits: {','.join(str(code) for code in exit_codes)}",
        f"Command:         {command_text}",
        "WARNING: parsed game count does not match requested count. Check interruption or PGN edits." if mismatch else "",
        "WARNING: CuteChess returned a non-zero exit code." if any(exit_codes) else "",
        "",
    ]
    body = "\n".join(header) + txt + "\n" + "=" * 64 + "\nGames\n" + "=" * 64 + "\n\n" + game_blocks_for_report(games)
    report_path.write_text(body, encoding="utf-8")
    write_csv(games, csv_path, args.player_name)
    print("\n" + txt)
    print(f"[Horizontal Rook] report: {report_path}")
    print(f"[Horizontal Rook] csv:    {csv_path}")
    print(f"[Horizontal Rook] pgn:    {pgn_path}")
    if protocol_path is not None:
        print(f"[Horizontal Rook] protocol: {protocol_path}")


def find_result_path(value: str, root: Path) -> Path:
    p = Path(value)
    if p.exists(): return p
    if not p.is_absolute():
        for cand in [root / value, root / "results" / value, root / "results" / f"{value}.txt", root / "results" / f"{value}_live.pgn"]:
            if cand.exists(): return cand
    return p


def latest_result(root: Path) -> Optional[Path]:
    res = root / "results"
    groups = [
        list(res.glob("match_result_*.txt")),
        [p for p in res.glob("*.txt") if not p.name.startswith("README")],
        list(res.glob("match_result_*_live.pgn")),
        list(res.glob("*.pgn")),
    ]
    for hits in groups:
        if hits:
            return max(hits, key=lambda p: p.stat().st_mtime)
    return None


def cmd_report(args) -> None:
    root = project_root()
    path = latest_result(root) if args.latest else find_result_path(args.path, root)
    if not path or not path.exists():
        raise SystemExit(f"File not found: {path}")
    games = parse_games(path)
    txt = report_text(games, args.player, args.opponent_elo, title=f"Horizontal Rook v1.10.0 Report - {path.name}")
    print(txt)
    if args.csv:
        csv_path = resolve_path(args.csv, root) or Path(args.csv)
        write_csv(games, csv_path, args.player)
        print(f"CSV written: {csv_path}")



def cmd_doctor(args) -> None:
    root = project_root()
    checks = [
        ("engine", root / "Eunshinbishop.exe"),
        ("cutechess", root / "external" / "cutechess-cli.exe"),
        ("stockfish", root / "external" / "stockfish.exe"),
        ("tools", root / "tools"),
        ("results", root / "results"),
        ("dll folder (optional but preserved)", root / ".dll"),
    ]
    print("=" * 64)
    print("Horizontal Rook doctor")
    print("=" * 64)
    print(f"project root: {root}")
    ok = True
    for name, path in checks:
        exists = path.exists()
        print(f"{'OK ' if exists else 'MISS'}  {name:<28} {path}")
        if name in {"engine", "cutechess", "stockfish", "tools"} and not exists:
            ok = False
    res = root / "results"
    if not res.exists():
        res.mkdir(parents=True, exist_ok=True)
        print(f"MADE results folder: {res}")
    print("")
    if ok:
        print("Verdict: ready. 수평룩 출근 가능.")
    else:
        print("Verdict: missing required files. 위 MISS부터 채워야 함.")


def cmd_list(args) -> None:
    root = project_root()
    res = root / "results"
    if not res.exists():
        print("No results folder yet.")
        return
    files = sorted([p for p in res.glob("*") if p.is_file()], key=lambda p: p.stat().st_mtime, reverse=True)
    print("=" * 64)
    print("Recent SniperBishop results")
    print("=" * 64)
    for p in files[:args.n]:
        print(f"{p.name:<45} {p.stat().st_size:>9} bytes")
    if not files:
        print("No result files yet. 수평룩으로 한 판 굴리면 생김.")


def cmd_quick(args) -> None:
    target = str(args.target).lower().replace("stockfish-", "").replace("sf", "")
    ns = argparse.Namespace(
        engine=args.engine,
        opponent=args.opponent,
        cutechess=args.cutechess,
        player_name=args.player_name,
        opponent_name="Stockfish-Full" if target in {"full", "fish", "stockfish"} else f"Stockfish-{target}",
        engine_options="",
        opponent_options="",
        limit_elo=None if target in {"full", "fish", "stockfish"} else int(target),
        opponent_elo=None if target in {"full", "fish", "stockfish"} else int(target),
        games=args.games,
        tc=args.tc,
        book=None,
        concurrency=args.concurrency,
        label=args.label or timestamp_label(f"full_stockfish_{args.color}_{args.games}g" if target in {"full", "fish", "stockfish"} else f"sf{target}_{args.color}_{args.games}g"),
        color=args.color,
        out_dir="results",
        prefix_match_result=False,
        resume=False,
    )
    run_match(ns)


def main() -> None:
    ap = argparse.ArgumentParser(description="Horizontal Rook v1.10.0 - paired, seeded SniperBishop match runner")
    sub = ap.add_subparsers(dest="cmd", required=True)

    run = sub.add_parser("run", help="run a CuteChess match")
    run.add_argument("--engine", default="Eunshinbishop.exe", help="default: .\\Eunshinbishop.exe")
    run.add_argument("--opponent", default="external/stockfish.exe", help="default: .\\external\\stockfish.exe")
    run.add_argument("--cutechess", default="external/cutechess-cli.exe", help="default: .\\external\\cutechess-cli.exe")
    run.add_argument("--player-name", default="SniperBishop")
    run.add_argument("--opponent-name", default="Stockfish")
    run.add_argument("--engine-options", default="")
    run.add_argument("--opponent-options", default="")
    run.add_argument("--limit-elo", type=int, default=None, help="shortcut: set Stockfish UCI_LimitStrength/UCI_Elo")
    run.add_argument("--opponent-elo", type=int, default=None)
    run.add_argument("--games", type=int, default=50)
    run.add_argument("--tc", default="10+0.1")
    run.add_argument("--book", default=None)
    run.add_argument("--book-format", choices=["pgn", "epd"], default=None)
    run.add_argument("--book-order", choices=["sequential", "random"], default="sequential")
    run.add_argument("--book-plies", type=int, default=None)
    run.add_argument("--seed", type=int, default=1)
    run.add_argument("--debug-log", action="store_true", help="save CuteChess -debug protocol output for node/depth auditing")
    run.add_argument("--concurrency", type=int, default=1)
    run.add_argument("--label", default=None)
    run.add_argument("--color", choices=["both", "white", "black"], default="both", help="focus SniperBishop color; black disables -repeat")
    run.add_argument("--out-dir", default="results")
    run.add_argument("--prefix-match-result", action="store_true", help="prefix custom label with match_result_")
    run.add_argument("--resume", action="store_true", help="append to an existing PGN/report label intentionally; default is fresh archive")
    run.set_defaults(func=run_match)

    fh = sub.add_parser("fishhunter", help="run against full Stockfish; report pure chess wins separately")
    fh.add_argument("--engine", default="Eunshinbishop.exe")
    fh.add_argument("--stockfish", default="external/stockfish.exe")
    fh.add_argument("--cutechess", default="external/cutechess-cli.exe")
    fh.add_argument("--player-name", default="SniperBishop")
    fh.add_argument("--opponent-name", default="Stockfish-Full")
    fh.add_argument("--games", type=int, default=30)
    fh.add_argument("--tc", default="10+0.1")
    fh.add_argument("--concurrency", type=int, default=1)
    fh.add_argument("--label", default=None)
    fh.add_argument("--color", choices=["both", "white", "black"], default="both")
    fh.add_argument("--out-dir", default="results")
    fh.add_argument("--resume", action="store_true", help="append to existing label intentionally; default is fresh archive")
    fh.set_defaults(func=lambda a: run_match(argparse.Namespace(**{**vars(a), "opponent": a.stockfish, "opponent_options": "", "engine_options": "", "opponent_elo": None, "book": None, "limit_elo": None, "prefix_match_result": False})))

    rep = sub.add_parser("report", help="analyze an existing PGN or match_result txt")
    rep.add_argument("path", nargs="?", default="")
    rep.add_argument("--latest", action="store_true", help="analyze latest .txt/.pgn in results")
    rep.add_argument("--player", default="SniperBishop")
    rep.add_argument("--opponent-elo", type=int, default=None)
    rep.add_argument("--csv", default=None)
    rep.set_defaults(func=cmd_report)

    quick = sub.add_parser("quick", help="shortcut: quick 2700 / quick 2800 / quick full")
    quick.add_argument("target", help="2700, 2800, 3000, or full")
    quick.add_argument("--games", type=int, default=30)
    quick.add_argument("--tc", default="10+0.1")
    quick.add_argument("--label", default=None)
    quick.add_argument("--concurrency", type=int, default=1)
    quick.add_argument("--engine", default="Eunshinbishop.exe")
    quick.add_argument("--opponent", default="external/stockfish.exe")
    quick.add_argument("--cutechess", default="external/cutechess-cli.exe")
    quick.add_argument("--player-name", default="SniperBishop")
    quick.add_argument("--color", choices=["both", "white", "black"], default="both", help="focus SniperBishop color")
    quick.set_defaults(func=cmd_quick)

    bt = sub.add_parser("blacktest", help="shortcut: test SniperBishop as Black only vs limited Stockfish")
    bt.add_argument("elo", type=int, nargs="?", default=2800)
    bt.add_argument("--games", type=int, default=20)
    bt.add_argument("--tc", default="10+0.1")
    bt.add_argument("--label", default=None)
    bt.add_argument("--concurrency", type=int, default=1)
    bt.add_argument("--engine", default="Eunshinbishop.exe")
    bt.add_argument("--opponent", default="external/stockfish.exe")
    bt.add_argument("--cutechess", default="external/cutechess-cli.exe")
    bt.add_argument("--player-name", default="SniperBishop")
    bt.set_defaults(func=lambda a: cmd_quick(argparse.Namespace(target=str(a.elo), games=a.games, tc=a.tc, label=a.label or f"sf{a.elo}_black_{a.games}g", concurrency=a.concurrency, engine=a.engine, opponent=a.opponent, cutechess=a.cutechess, player_name=a.player_name, color="black")))

    doctor = sub.add_parser("doctor", help="check expected files/folders")
    doctor.set_defaults(func=cmd_doctor)

    ls = sub.add_parser("list", help="list recent files in results")
    ls.add_argument("-n", type=int, default=20)
    ls.set_defaults(func=cmd_list)

    latest = sub.add_parser("latest", help="analyze latest results file")
    latest.add_argument("--player", default="SniperBishop")
    latest.add_argument("--opponent-elo", type=int, default=None)
    latest.set_defaults(func=lambda a: cmd_report(argparse.Namespace(path="", latest=True, player=a.player, opponent_elo=a.opponent_elo, csv=None)))

    args = ap.parse_args()
    args.func(args)

if __name__ == "__main__":
    main()

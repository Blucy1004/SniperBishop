#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
Backing Knight v1.8.2 (VictoryAutopsy + SafeStopAware)
"나이트는 원래 뒤로가요" - SniperBishop v3.2 post-match reviewer.

Tool package stamp: v3.2.1 SafeStop tools-fix 20260710. Unfinished games are ignored instead of being counted as losses.

Fixes:
  - latest scans results recursively and only picks parseable .txt/.pgn game files
  - player defaults to auto, so older EunshinBishop-K26 reports do not become 0 games
  - VictoryAutopsy: wins are analyzed too, so 10-0 reports do not become empty applause
"""
from __future__ import annotations

import argparse
import glob
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional

TAG_RE = re.compile(r'\[(\w+)\s+"([^"]*)"\]')
GAME_BLOCK_RE = re.compile(r'(?:--- Game \d+ ---\s*)?(\[Event.*?)(?=\n--- Game \d+ ---|\n\[Event |\Z)', re.S)
MOVE_RE = re.compile(r'\b\d+\.\.\.|\b\d+\.|\{[^}]*\}|\$\d+')
ENGINE_HINTS = ("sniper", "eunshin", "bishop", "k26", "dalgae")
OPPONENT_HINTS = ("stockfish", "carlsen", "magnus", "komodo", "leela", "lc0", "dragon")


def project_root() -> Path:
    return Path(__file__).resolve().parents[1]


def name_key(s: str) -> str:
    return (s or "").lower().replace(" ", "").replace("-", "").replace("_", "")


def name_match(needle: str, haystack: str) -> bool:
    if not needle or not haystack or needle.lower() == "auto":
        return False
    a = name_key(needle)
    b = name_key(haystack)
    return bool(a and b and (a in b or b in a))


@dataclass
class Game:
    source: str
    index: int
    text: str
    tags: Dict[str, str]

    @property
    def white(self): return self.tags.get("White", "?")
    @property
    def black(self): return self.tags.get("Black", "?")
    @property
    def result(self): return self.tags.get("Result", "*")
    @property
    def opening(self):
        op = self.tags.get("Opening", "?")
        var = self.tags.get("Variation", "")
        return op if not var else f"{op} / {var}"
    @property
    def ply(self):
        try: return int(self.tags.get("PlyCount", "0"))
        except ValueError: return 0
    @property
    def termination(self): return self.tags.get("Termination", "")

    def player_color(self, player: str) -> Optional[str]:
        if name_match(player, self.white): return "White"
        if name_match(player, self.black): return "Black"
        return None

    def winner(self) -> Optional[str]:
        if self.result == "1-0": return self.white
        if self.result == "0-1": return self.black
        return None

    def is_finished(self) -> bool:
        return self.result in {"1-0", "0-1", "1/2-1/2"}

    def score_for(self, player: str) -> Optional[float]:
        if not self.player_color(player) or not self.is_finished(): return None
        if self.result == "1/2-1/2": return 0.5
        w = self.winner()
        return 1.0 if w and name_match(player, w) else 0.0

    def time_forfeit(self) -> bool:
        lower = (self.termination + "\n" + self.text).lower()
        return "time forfeit" in lower or "loses on time" in lower or "forfeit" in lower

    def bad_marker(self) -> bool:
        lower = self.text.lower()
        return any(x in lower for x in ["illegal", "position_desync", "desync", "bestmove 0000", " 0000"])


def parse_games(path: Path) -> List[Game]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    blocks = [b.strip() for b in GAME_BLOCK_RE.findall(text) if b.strip()]
    if not blocks and "[Event" in text:
        blocks = [p.strip() for p in re.split(r'(?=\[Event )', text) if p.strip().startswith("[Event")]
    games: List[Game] = []
    for i, b in enumerate(blocks, 1):
        tags = dict(TAG_RE.findall(b))
        if tags:
            games.append(Game(path.name, i, b, tags))
    return games


def is_probably_opponent(name: str) -> bool:
    k = name_key(name)
    return any(x in k for x in OPPONENT_HINTS)


def detect_player(games: List[Game], requested: str = "auto") -> str:
    if requested and requested.lower() != "auto":
        if any(g.player_color(requested) for g in games):
            return requested
        print(f"[Backing Knight] player '{requested}' not found in tags; auto-detecting player name.")
    names: Dict[str, int] = {}
    original: Dict[str, str] = {}
    for g in games:
        for n in [g.white, g.black]:
            if not n or n == "?":
                continue
            k = name_key(n)
            names[k] = names.get(k, 0) + 1
            original.setdefault(k, n)
    hinted = [(cnt, original[k]) for k, cnt in names.items() if any(h in k for h in ENGINE_HINTS)]
    if hinted:
        hinted.sort(reverse=True)
        return hinted[0][1]
    non_opp = [(cnt, original[k]) for k, cnt in names.items() if not is_probably_opponent(original[k])]
    if non_opp:
        non_opp.sort(reverse=True)
        return non_opp[0][1]
    return original[next(iter(names))] if names else "SniperBishop"


def result_candidates(root: Path) -> List[Path]:
    res = root / "results"
    if not res.exists():
        return []
    files: List[Path] = []
    for ext in ("*.txt", "*.pgn"):
        files.extend(res.rglob(ext))
    skip_tokens = ("readme", "backing_knight_")
    files = [p for p in files if p.is_file() and not any(tok in p.name.lower() for tok in skip_tokens)]
    return sorted(files, key=lambda p: p.stat().st_mtime, reverse=True)


def latest_result(root: Path) -> Optional[Path]:
    for p in result_candidates(root):
        if parse_games(p):
            return p
    return None


def materialize_paths(patterns: List[str], root: Path, latest: bool = False) -> List[Path]:
    if latest or not patterns:
        p = latest_result(root)
        return [p] if p else []
    out: List[Path] = []
    res = root / "results"
    for pat in patterns:
        raw = Path(pat)
        candidates = [str(raw)]
        if not raw.is_absolute():
            candidates += [str(root / pat), str(res / pat), str(res / f"{pat}.txt"), str(res / f"{pat}_live.pgn")]
            if res.exists():
                candidates += [str(p) for p in res.rglob(pat)]
                candidates += [str(p) for p in res.rglob(f"{pat}.txt")]
                candidates += [str(p) for p in res.rglob(f"{pat}_live.pgn")]
        found = False
        for c in candidates:
            hits = glob.glob(c)
            if hits:
                out.extend(Path(h) for h in hits)
                found = True
                break
        if not found and raw.exists():
            out.append(raw)
    seen = set(); dedup: List[Path] = []
    for p in out:
        p = p.resolve()
        if p not in seen and p.exists():
            seen.add(p); dedup.append(p)
    return dedup


def last_moves(game: Game, n_tokens: int = 30) -> str:
    body = game.text.split("\n\n", 1)[-1]
    body = re.sub(r'\{[^}]*\}', '', body)
    body = re.sub(r'\[[^\]]*\]', '', body)
    tokens = [t for t in body.replace("\n", " ").split() if t not in ["1-0", "0-1", "1/2-1/2", "*"]]
    return " ".join(tokens[-n_tokens:])


def first_mate_ply(game: Game) -> Optional[int]:
    m = re.search(r'\{[+-]?M\d+/', game.text)
    if not m: return None
    before = game.text[:m.start()]
    body = before.split("\n\n", 1)[-1]
    body = MOVE_RE.sub(' ', body)
    toks = [t for t in body.replace("\n", " ").split() if t and t not in ["1-0", "0-1", "1/2-1/2", "*"]]
    return len(toks)


def opponent_name(game: Game, player: str) -> str:
    c = game.player_color(player)
    if c == "White": return game.black
    if c == "Black": return game.white
    return "?"


def classify_loss(game: Game, player: str) -> List[str]:
    tags: List[str] = []
    if game.time_forfeit(): tags.append("time-forfeit")
    if game.bad_marker(): tags.append("bad-marker")
    if game.ply and game.ply <= 80: tags.append("opening/early collapse")
    if game.ply and game.ply >= 120: tags.append("long-game/endgame collapse")
    text = game.text
    if re.search(r'=[QRBN]', text): tags.append("promotion race")
    if "#" in text: tags.append("mated")
    op = game.opening.lower()
    if "french" in op and ("chatard" in op or "albin" in op): tags.append("French Chatard danger")
    if "ruy lopez" in op: tags.append("Ruy Lopez danger")
    if "giuoco" in op or "italian" in op: tags.append("Italian/Giuoco pressure")
    if "gambit" in op: tags.append("gambit pressure")
    tail = last_moves(game, 50)
    if re.search(r'Q[a-h1-8x+#=]*|R[a-h1-8x+#=]*', tail): tags.append("queen/rook invasion")
    if re.search(r'=[Q]', tail): tags.append("late promotion finish")
    return tags or ["uncategorized"]


def classify_win(game: Game, player: str) -> List[str]:
    tags: List[str] = []
    if game.time_forfeit(): tags.append("time-forfeit win")
    if game.bad_marker(): tags.append("bad-marker/noise win")
    if not game.time_forfeit() and not game.bad_marker(): tags.append("pure chess win")
    if "#" in game.text or "mate" in game.termination.lower():
        tags.append("mate-net finish")
    if game.ply:
        if game.ply <= 50:
            tags.append("fast tactical kill")
        elif game.ply <= 90:
            tags.append("midgame collapse punish")
        elif game.ply >= 120:
            tags.append("long conversion")
    text = game.text
    if re.search(r'=[QRBN]', text): tags.append("promotion conversion")
    op = game.opening.lower()
    if any(x in op for x in ["van't kruijs", "vant kruijs", "alapin", "reti", "queen's pawn"]):
        tags.append("passive/loose opening punished")
    if "gambit" in op:
        tags.append("gambit accepted/punished")
    tail = last_moves(game, 50)
    if re.search(r'Q[a-h1-8x+#=]*|R[a-h1-8x+#=]*', tail): tags.append("major-piece invasion")
    if re.search(r'N[a-h1-8x+#=]*|B[a-h1-8x+#=]*', tail): tags.append("minor-piece finish/support")
    opp = opponent_name(game, player).lower()
    if "stockfish-100" in opp or "sf100" in game.source.lower():
        tags.append("PotatoFish mode")
    return tags or ["uncategorized win"]


def collapse_note(game: Game, player: str) -> str:
    tags = classify_win(game, player)
    if "time-forfeit win" in tags:
        return "시간승이라 블런더쇼 판단 보류"
    if "mate-net finish" in tags and "passive/loose opening punished" in tags:
        return "느슨한 초반 + 왕 안전 방치 → 메이트망"
    if "fast tactical kill" in tags:
        return "초반 전술/왕 안전 붕괴"
    if "midgame collapse punish" in tags:
        return "중반 방어 우선순위 실패"
    if "long conversion" in tags:
        return "오래 버텼지만 끝내기/왕 안전에서 붕괴"
    if "mate-net finish" in tags:
        return "메이트 위협을 늦게 감지"
    return "명확한 단일 패턴은 약함"


def build_report(games: List[Game], player: str) -> str:
    player = detect_player(games, player)
    target_all = [g for g in games if g.player_color(player)]
    unfinished = [g for g in target_all if not g.is_finished()]
    target = [g for g in target_all if g.is_finished()]
    wins = [g for g in target if g.score_for(player) == 1.0]
    losses = [g for g in target if g.score_for(player) == 0.0]
    draws = [g for g in target if g.score_for(player) == 0.5]
    pure_wins = [g for g in wins if not g.time_forfeit() and not g.bad_marker()]
    time_wins = [g for g in wins if g.time_forfeit()]

    color = {"White": [0,0,0], "Black": [0,0,0]}
    for g in target:
        c = g.player_color(player)
        sc = g.score_for(player)
        if c:
            if sc == 1.0: color[c][0] += 1
            elif sc == 0.5: color[c][2] += 1
            elif sc == 0.0: color[c][1] += 1

    opening_counter = Counter(g.opening for g in target)
    losing_openings = Counter(g.opening for g in losses)
    tag_counter = Counter()
    examples = defaultdict(list)
    for g in losses:
        for t in classify_loss(g, player):
            tag_counter[t] += 1
            if len(examples[t]) < 3:
                examples[t].append(g)

    lines: List[str] = []
    lines.append("# Backing Knight v1.8.2 Report")
    lines.append("")
    lines.append("> 나이트는 원래 뒤로가요. 이제 이긴 판도 되감아서 상대가 어디서 감자됐는지 봅니다.")
    lines.append("")
    lines.append(f"Player: **{player}**")
    lines.append(f"Parsed games in file: **{len(games)}**")
    lines.append(f"Finished games analyzed for player: **{len(target)}**")
    if unfinished:
        lines.append(f"Unfinished/ignored games: **{len(unfinished)}**")
    lines.append(f"Score: **{len(wins)}W {len(losses)}L {len(draws)}D**")
    total = len(target)
    if total:
        pct = 100 * (len(wins) + 0.5 * len(draws)) / total
        lines.append(f"Score rate: **{pct:.1f}%**")
    lines.append(f"Pure chess wins: **{len(pure_wins)}**")
    lines.append(f"Time-forfeit wins: **{len(time_wins)}**")
    lines.append("")
    lines.append("## Color split")
    color_pct = {}
    for c, (w,l,d) in color.items():
        n = w+l+d
        pct = 100*(w+0.5*d)/n if n else 0
        color_pct[c] = pct
        lines.append(f"- {c}: {w}W {l}L {d}D ({pct:.1f}%)")
    if sum(color["White"]) >= 4 and sum(color["Black"]) >= 4:
        gap = color_pct["White"] - color_pct["Black"]
        if gap >= 35:
            lines.append(f"- ⚠️ **Black emergency:** White is ahead by {gap:.1f}%p. Patch black repertoire/counterplay first; do not nerf White attack.")
        elif gap <= -35:
            lines.append(f"- ⚠️ **White emergency:** Black is ahead by {-gap:.1f}%p. White repertoire got too quiet.")
    win_tag_counter = Counter()
    win_examples = defaultdict(list)
    for g in wins:
        for t in classify_win(g, player):
            win_tag_counter[t] += 1
            if len(win_examples[t]) < 3:
                win_examples[t].append(g)
    fastest_wins = sorted(wins, key=lambda g: g.ply or 999999)[:5]
    longest_wins = sorted(wins, key=lambda g: g.ply, reverse=True)[:5]

    lines.append("")
    lines.append("## Victory autopsy")
    if not wins:
        lines.append("- No wins to autopsy. 오늘은 장례식장이 아니라 응급실임.")
    else:
        lines.append(f"- Wins analyzed: **{len(wins)}**")
        if win_tag_counter:
            lines.append("- Main win patterns:")
            for tag, cnt in win_tag_counter.most_common(10):
                lines.append(f"  - **{tag}**: {cnt}")
        mate_wins = sum(1 for g in wins if "#" in g.text or "mate" in g.termination.lower())
        if mate_wins:
            lines.append(f"- Mate finishes: **{mate_wins}/{len(wins)}**")
        if fastest_wins:
            lines.append("- Fastest wins:")
            for g in fastest_wins[:3]:
                lines.append(f"  - Game {g.index}: {g.ply} ply, {g.opening} → {collapse_note(g, player)}")
        if longest_wins:
            lines.append("- Longest conversions:")
            for g in longest_wins[:3]:
                lines.append(f"  - Game {g.index}: {g.ply} ply, {g.opening} → {collapse_note(g, player)}")

    lines.append("")
    lines.append("## PotatoFish blunder show")
    potato = any("PotatoFish mode" in classify_win(g, player) for g in wins)
    perfect_small = len(target) and len(wins) == len(target) and len(target) <= 20
    if wins and (potato or perfect_small):
        lines.append("- 상대가 기물을 대놓고 던졌는지는 PGN 세부수로 더 봐야 하지만, 리포트 기준 핵심은 **전술/메이트 방어 실패**임.")
        lines.append("- 시간승이 아니라 순수 체스승이 많으면, SniperBishop이 실수를 실제 메이트/전술로 응징했다는 뜻.")
        if win_tag_counter["passive/loose opening punished"]:
            lines.append(f"- 느슨한/수동적 오프닝 응징 후보: **{win_tag_counter['passive/loose opening punished']}**판")
        if win_tag_counter["mate-net finish"]:
            lines.append(f"- 메이트망 완성 후보: **{win_tag_counter['mate-net finish']}**판")
        lines.append("- 판정: **마틴식 기물 헌납형보단, 문단속 실패형 감자일 가능성이 큼.**")
    elif wins:
        lines.append("- 감자피쉬 전용 표본은 아니지만, 이긴 판의 붕괴 패턴은 위 Victory autopsy 참고.")
    else:
        lines.append("- 승리가 없어서 블런더쇼 없음.")
    lines.append("")
    lines.append("## Most common loss patterns")
    if not games:
        lines.append("- No PGN games parsed. LatestFinder picked no game file.")
    elif not target:
        lines.append("- Games were parsed, but player name did not match. Use `--player auto` or the exact PGN name.")
    elif tag_counter:
        for tag, cnt in tag_counter.most_common(10):
            lines.append(f"- **{tag}**: {cnt}")
    else:
        lines.append("- No losses found. 수평룩이 박수치고 있음.")
    lines.append("")
    lines.append("## Dangerous openings")
    if losing_openings:
        for op, cnt in losing_openings.most_common(10):
            total_op = opening_counter[op]
            lines.append(f"- {op}: {cnt}/{total_op} losses")
    else:
        lines.append("- None or no matching losses.")
    lines.append("")
    lines.append("## Example failures")
    if examples:
        for tag, gs in list(examples.items())[:8]:
            lines.append(f"### {tag}")
            for g in gs:
                mate_ply = first_mate_ply(g)
                mate_note = f", first mate eval around ply {mate_ply}" if mate_ply else ""
                lines.append(f"- {g.source} Game {g.index}: {g.white} vs {g.black}, {g.result}, ply {g.ply}, {g.opening}{mate_note}")
                lines.append(f"  - last line: `{last_moves(g, 18)}`")
    else:
        lines.append("- None")
    lines.append("")
    lines.append("## Patch suggestions")
    suggestions: List[str] = []
    if sum(color["White"]) >= 4 and sum(color["Black"]) >= 4 and color_pct.get("White", 0) - color_pct.get("Black", 0) >= 35:
        suggestions.append("BlackCounter 우선: 흑 오프닝/반격만 수리하고, 백 공격 보너스는 건드리지 말 것.")
    if tag_counter["opening/early collapse"]:
        suggestions.append("오프닝 자살 라인만 핀셋 제거. 엔진 성격 전체를 수비형으로 바꾸지 말 것.")
    if tag_counter["French Chatard danger"]:
        suggestions.append("French Chatard 계열에서 왕이 직접 끌려나오는 라인 검문.")
    if tag_counter["Ruy Lopez danger"]:
        suggestions.append("Ruy Lopez 흑에서 ...Ng4/...Nxf2 계열 희생은 SEE/강제수 검증 후 허용.")
    if tag_counter["Italian/Giuoco pressure"]:
        suggestions.append("Giuoco/Italian에서 초반 퀸사이드/중앙 고정 후 룩 침투 당하는 패턴 확인.")
    if tag_counter["promotion race"]:
        suggestions.append("passed pawn race 평가와 promotion extension 강화.")
    if tag_counter["long-game/endgame collapse"]:
        suggestions.append("rook/pawn endgame 생존 평가 추가. 단, 공격 보너스를 깎지는 말 것.")
    if tag_counter["queen/rook invasion"]:
        suggestions.append("상대 퀸/룩 침투는 최소 방어 검문만 추가. 과한 king-safety 패널티 금지.")
    if not suggestions:
        if wins and not losses:
            suggestions.append("전승 리포트는 약점보다 강점 확인용. 다음은 1500→2000→2400→2600→2800 계단으로 흑 붕괴선을 찾기.")
            if win_tag_counter["mate-net finish"]:
                suggestions.append("메이트망 응징력은 유지. BlackCounter 패치에서도 이 공격 코어는 건드리지 말 것.")
        else:
            suggestions.append("큰 반복 패턴이 안 보임. 수평룩으로 더 많은 판을 모아서 다시 복기.")
    for i, s in enumerate(suggestions, 1):
        lines.append(f"{i}. {s}")
    lines.append("")
    lines.append("## Backing Knight verdict")
    if not games:
        lines.append("리포트 파일을 못 읽음. `python .\\tools\\horizontal_rook.py list --all`로 실제 게임 파일을 확인.")
    elif not target:
        lines.append("선수명 매칭 실패. v1.8.1에서는 보통 auto로 해결됨. 그래도 안 되면 PGN의 White/Black 이름을 확인.")
    elif wins and not losses and len(target) < 20:
        lines.append("전승이지만 표본은 작음. 그래도 기본 작동/응징력 확인용으로는 의미 있음. 다음은 더 강한 상대 계단 테스트.")
    elif len(target) < 20:
        lines.append("표본이 작음. 결론은 보류, 그래도 패턴 후보는 참고 가능.")
    elif len(pure_wins) == 0 and len(time_wins) > 0:
        lines.append("전적표 승리는 있으나 체스승이 부족함. 공격 정확도 또는 시간관리 둘 중 하나를 따로 점검.")
    elif len(losses) > len(wins) * 2:
        lines.append("현재 상대가 확실히 강함. 약점 패턴을 하나씩 제거하는 쪽이 맞음.")
    else:
        lines.append("실전 가능. 다음 패치는 수평룩으로 이전 버전 대비 검증.")
    return "\n".join(lines) + "\n"


def auto_save_path(paths: List[Path], root: Path) -> Path:
    stem = paths[0].stem if paths else "latest"
    if stem.endswith("_live"):
        stem = stem[:-5]
    return root / "results" / f"backing_knight_{stem}.md"


def main() -> None:
    ap = argparse.ArgumentParser(description="Backing Knight v1.8.1 - SniperBishop v3.2 TimeSniper reviewer + victory autopsy")
    ap.add_argument("paths", nargs="*", help="PGN or match_result files; glob patterns allowed. Empty = latest results file.")
    ap.add_argument("--latest", action="store_true", help="use latest parseable .txt/.pgn under results recursively")
    ap.add_argument("--player", default="auto")
    ap.add_argument("--save", nargs="?", const="AUTO", default=None, help="optional markdown output path. Use --save alone for automatic results/backing_knight_*.md")
    args = ap.parse_args()

    root = project_root()
    paths = materialize_paths(args.paths, root, latest=args.latest or not args.paths)
    if not paths:
        raise SystemExit("No game result files found. Put match reports/PGNs in .\\results or pass a path.")
    games: List[Game] = []
    for p in paths:
        parsed = parse_games(p)
        print(f"[Backing Knight] using file: {p} ({len(parsed)} games)")
        games.extend(parsed)
    player = detect_player(games, args.player)
    print(f"[Backing Knight] player: {player}")
    report = build_report(games, player)
    print(report)
    if args.save is not None:
        out = auto_save_path(paths, root) if args.save == "AUTO" else Path(args.save)
        if not out.is_absolute(): out = root / out
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(report, encoding="utf-8")
        print(f"Backing Knight report saved: {out}")


if __name__ == "__main__":
    main()

"""The 120 Hz mode (runtime/hfr.c): is a battle at frame_rate = 120 deterministic? Compares runs of the same scripted
fight in the deterministic mode, two files per run:
  - the fighters' per-fight-frame state log (SSBU_FIGHT_HASH=<file>, fighter_sdk/fighter_host.c: position, status,
    motion, motion frame, damage, the script random generators and a running hash over all of it);
  - the 120 Hz mode's per-update log (SSBU_HFR_TRACE=<file>, hfr.c: cycle counter, the cycle's divisor, the asked
    divisor, the update within the cycle, added or not, the update rate, the match timer, a hash of the generators).
Neither holds a time: two runs of the same build, sequence and options must give the same lines, whatever the host's
speed (SSBU_SPEED=0 uncapped one refresh per frame, or real time), the render load (resolution scale) or the moment of
the run. A real-time run needs SSBU_DETERMINISTIC=1 for that (it clears the game's catch-up updates for refreshes it
missed: their number is wall time, at 60 Hz as at 120).

    python tests/hfr_det.py <run A> <run B> [<run C> ...]       every run against A: must be IDENTICAL
    python tests/hfr_det.py --fight-only <run A> <run B> ...     the fight logs only (frame_rate = 60 runs have no trace:
                                                                 this branch at 60 against its base must be identical)
    python tests/hfr_det.py --cross <run 60> <run 120>          60 against 120: first difference only, informative

A run is a tag of tests/s11_run.py (files tests/logs/s11_<tag>.fh.txt and .hfr.txt) or the path of a hash file (the
trace is looked for beside it as <name>.hfr.txt, or hfr_trace.txt in the same folder). Runs for it, e.g.:

    python tests/s11_run.py h120a - seq_n4_det.txt 1400 SSBU_FRAME_RATE=120 SSBU_DETERMINISTIC=1 \
        SSBU_DEBUG_MATCH=p1=mario,p2=donkey,stage=battle_field,cpu=9,stock=3 SSBU_HFR_TRACE=tests/logs/s11_h120a.hfr.txt
    (the same as h120b; h120rt with SSBU_SPEED=(unset); h120s2 with SSBU_VK_SCALE=2.0)
    python tests/hfr_det.py h120a h120b h120rt h120s2

Exit code 0 when every comparison asked for passed (with --cross: always 0). No game content in this file."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.join(HERE, 'logs')
MIN_LINES = 500          # fewer common fight-log lines than this is no evidence


def paths(run):
    if os.path.exists(run):
        fh = run
        base = fh[:-7] if fh.endswith('.fh.txt') else os.path.splitext(fh)[0]
        for tr in (base + '.hfr.txt', os.path.join(os.path.dirname(fh), 'hfr_trace.txt')):
            if os.path.exists(tr):
                return fh, tr
        return fh, None
    fh = os.path.join(LOGS, 's11_%s.fh.txt' % run)
    tr = os.path.join(LOGS, 's11_%s.hfr.txt' % run)
    return fh, tr if os.path.exists(tr) else None


def lines(path, keep):
    if not path or not os.path.exists(path):
        return None
    with open(path, errors='replace') as f:
        out = [l.rstrip('\n') for l in f if keep(l)]
    if out and not out[-1].endswith(')') and out[-1].startswith('F '):
        out.pop()                                       # a killed run leaves a cut last line
    return out


def fh_lines(path):
    return lines(path, lambda l: l.startswith('F ') and ' H ' in l)


def tr_lines(path):
    t = lines(path, lambda l: l.startswith('u '))
    if t and len(t[-1].split()) != 18:
        t.pop()                                         # cut last line
    return t


def fields(line):
    p = line.split('(')[0].split()
    return {p[i]: p[i + 1] for i in range(0, len(p) - 1, 2)}


def compare(name, a, b, what, tag_a, tag_b, min_lines):
    if a is None or b is None:
        print('  %s: MISSING (%s: %s, %s: %s)' % (what, tag_a, 'ok' if a is not None else 'no file', tag_b, 'ok' if b is not None else 'no file'))
        return False, None
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            fa, fb = fields(a[i]), fields(b[i])
            diff = [k for k in fa if k in fb and fa[k] != fb[k] and k != 'H']
            print('  %s: FIRST DIFFERENCE at line %d of %d / %d: fields %s' % (what, i + 1, len(a), len(b), ' '.join(diff[:10]) or '(running hash only)'))
            for k in range(max(0, i - 2), min(n, i + 3)):
                print('     %-8s %s' % (tag_a, a[k][:200]))
                print('     %-8s %s' % (tag_b, b[k][:200]))
            return False, i
    ok = n >= min_lines
    print('  %s: %s over %d common lines (%d / %d)%s' % (what, 'IDENTICAL' if ok else 'identical but TOO SHORT', n, len(a), len(b), '' if ok else ' (fewer than %d)' % min_lines))
    return ok, None


def summary(tag, fh, tr):
    if fh:
        last = fh[-1].split()[1] if fh else '-'
        ent = sorted({fields(l).get('e') for l in fh})
        print('  %-8s fight log: %d lines, last fight frame %s, fighters %s, final running hash %s' % (tag, len(fh), last, ','.join(e for e in ent if e), fields(fh[-1]).get('H', '-') if fh else '-'))
    if tr:
        added = sum(1 for l in tr if fields(l).get('add') == '1')
        rates = sorted({fields(l).get('rate') for l in tr})
        ns = sorted({int(fields(l).get('n', 0)) for l in tr})
        print('  %-8s hfr trace: %d battle updates, %d added (%.1f %%), cycle divisors %s, update rates %s' % (tag, len(tr), added, 100.0 * added / max(1, len(tr)), ns, ','.join(rates)))


def main():
    args = sys.argv[1:]
    cross, fight_only = '--cross' in args, '--fight-only' in args
    args = [a for a in args if a not in ('--cross', '--fight-only')]
    if len(args) < 2:
        print(__doc__)
        return 2
    label = lambda a: os.path.basename(os.path.dirname(os.path.abspath(a))) if os.path.basename(a) == 'fh.txt' else os.path.basename(a).replace('.fh.txt', '')
    runs = [(label(a) if os.path.exists(a) else a,) + paths(a) for a in args]
    data = [(tag, fh_lines(fh), tr_lines(tr)) for tag, fh, tr in runs]
    for tag, fh, tr in data:
        summary(tag, fh, tr)
    if cross:       # 60 against 120: the fight logs are in fight frames (logic), so they can be compared line by line
        tag_a, fa, _ = data[0]
        tag_b, fb, _ = data[1]
        print('%s (60) vs %s (120), informative:' % (tag_a, tag_b))
        compare('fight log', fa, fb, 'fight log', tag_a, tag_b, 0)
        return 0
    ok_all = True
    tag_a, fa, ta = data[0]
    for tag_b, fb, tb in data[1:]:
        print('%s vs %s:' % (tag_a, tag_b))
        ok1, _ = compare('fight', fa, fb, 'fight log', tag_a, tag_b, MIN_LINES)
        ok2 = fight_only or compare('trace', ta, tb, 'hfr trace', tag_a, tag_b, MIN_LINES)[0]
        ok_all = ok_all and ok1 and ok2
    print('RESULT: %s' % ('DETERMINISTIC (every run identical to %s)' % tag_a if ok_all else 'NOT shown deterministic'))
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Measure the entire historical Vulkan prewarm without starting the guest game.

The instrumented runtime's SSBU_VK_PREWARM_BENCH path skips background container
preparation and exits without saving caches. Run serially, on an unchanged cache.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[1] / 'build')
    ap.add_argument('--exe', required=True, help='Executable name inside build/work')
    ap.add_argument('--cache', type=Path, required=True)
    ap.add_argument('--workers', type=int, choices=range(1, 17), default=16)
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--no-profile', action='store_true', help='Check measurement overhead')
    a = ap.parse_args()
    build, cache, log = a.build.resolve(), a.cache.resolve(), a.log.resolve()
    state = json.loads((build / 'state.json').read_text())
    log.parent.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('SSBU_', 'RECOMP_'))}
    env.update(SSBU_ROMFS=state['dump']['romfs'], SSBU_SHOW='0', SSBU_RENDERER='vk',
               SSBU_VK_CACHE=str(cache), SSBU_VK_WORKERS=str(a.workers),
               SSBU_VK_PREWARM_BENCH='1', SSBU_VK_PREWARM_PROFILE='0' if a.no_profile else '1')
    with tempfile.TemporaryDirectory(prefix='prewarm-', dir=log.parent) as scratch:
        env['SSBU_USERDATA'] = scratch
        env['SSBU_TRACE'] = str(Path(scratch) / 'boot_trace.txt')
        started = time.perf_counter()
        timed_out = False
        with log.open('w', encoding='utf-8') as output:
            try:
                process = subprocess.run([str(build / 'work' / a.exe),
                                          str(build / 'binaries/13.0.4/main.elf')],
                                         cwd=build / 'work', env=env, stdout=output,
                                         stderr=subprocess.STDOUT, timeout=a.timeout)
                exit_code = process.returncode
            except subprocess.TimeoutExpired:
                timed_out, exit_code = True, None
        elapsed = time.perf_counter() - started
    text = log.read_text(encoding='utf-8', errors='replace')
    match = re.search(r'pipelines made ahead: (\d+) of (\d+) listed \((\d+) stages from their records, '
                      r'(\d+) entries without their stage files\) in (\d+) ms on (\d+) threads; '
                      r'the first batch waited (\d+) ms', text)
    summary = dict(exe=a.exe, workers=a.workers, cache=str(cache), log=str(log),
                   exit_code=exit_code, timed_out=timed_out, process_seconds=round(elapsed, 3))
    if match:
        summary.update(zip(('made', 'listed', 'stages', 'missing', 'prewarm_ms', 'threads', 'wait_ms'),
                           map(int, match.groups())))
    summary['complete'] = exit_code == 0 and match is not None and 'nvn_vk_prewarm_bench: complete' in text
    log.with_suffix('.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))
    for line in text.splitlines():
        if line.startswith('nvn_vk_prewarm_profile:'):
            print(line)
    return 0 if summary['complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

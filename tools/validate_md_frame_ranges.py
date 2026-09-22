"""Compare complete-command interval/range selection and counter scopes with Python."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
from types import SimpleNamespace


class Counter:
    def __init__(self):
        self.trace = []; self.fault = ''; self.active = False
        self.serial = 0; self.current = None; self.pending = []
    def fail(self, name):
        if self.fault == name: raise RuntimeError('Injected ' + name)
    def begin(self, consume=None):
        self.trace.append(['begin', bool(consume)]); self.fail('begin')
        if self.active: raise RuntimeError('Already active')
        self.active = True; self.serial += 1; self.current = consume
    def submit(self):
        self.trace.append(['submit', self.serial]); self.fail('submit')
        self.pending.append((self.serial, self.current)); self.current = None; self.active = False
    def value(self, token): return dict(token=token), bytes([token % 256])
    def end(self):
        self.trace.append(['end', self.serial]); self.fail('end')
        value = self.value(self.serial); self.active = False
        if self.current: self.current(*value)
        self.current = None
        return value
    def flush(self):
        pending = self.pending; self.pending = []
        for token, consume in pending:
            self.trace.append(['deliver', token])
            if consume: consume(*self.value(token))


def write_capture(path, entries):
    data = bytearray(0x128); table = []
    for eid, category, kind in entries:
        payload = struct.pack('<QQ', 0, 1)
        table.append((eid, len(data), len(payload), 0, category, kind)); data.extend(payload)
    struct.pack_into('<IIII', data, 0, 0x41504749, 0x128, 3, len(table))
    data[0x44:0x48] = b'DX11'
    struct.pack_into('<Q', data, 0xf4, len(data))
    for entry in table: data.extend(struct.pack('<QQIBBH', *entry))
    path.write_bytes(data)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('reference', 'exe', 'qt-bin', 'out'): p.add_argument('--' + key, type=Path, required=True)
    p.add_argument('--isolated-env', action='store_true'); args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference / 'standalone'))
    from frame import Frame
    from md_interval import selection, IntervalCounter
    from md_frame_ranges import select_frame_ranges, FrameRangeCounter

    def reference(job):
        op = job['op']
        if op in ('interval_selection', 'frame_selection'):
            with Frame(job['capture']) as frame:
                experiment = SimpleNamespace(events={i: dict(enabled=False) for i in job.get('disabled', [])})
                if op == 'interval_selection': return selection(frame, experiment, job.get('start'), job.get('end'))
                return select_frame_ranges(frame, job.get('indices'), experiment)
        native = Counter()
        def consume(*args):
            if op == 'interval': result, raw = args; row = ['consume', result, list(raw)]
            else: info, result, raw = args; row = ['consume', info['range_index'], result, list(raw)]
            native.trace.append(row); native.fail('consume')
        counter = (IntervalCounter(native, job['interval'], consume, callback_reports=job['callbacks']) if op == 'interval'
            else FrameRangeCounter(native, job['ranges'], consume, callback_reports=job['callbacks']))
        output = []
        for step in job['steps']:
            row = {}; value = None
            try:
                if step['op'] == 'command':
                    with counter.scope(None, SimpleNamespace(id=step['event'])):
                        native.trace.append(['command', step['event']])
                        if step.get('fail', False): raise RuntimeError('Injected command')
                elif step['op'] == 'fault': native.fault = step['value']
                elif step['op'] == 'flush': native.flush()
                elif step['op'] == 'verify': value = counter.verify()
                row['return'] = value
            except Exception as exc: row['error'] = str(exc)
            fields = ('seen', 'completed', 'active', 'failed') if op == 'interval' else ('position', 'seen', 'audit', 'active', 'failed', 'last_event')
            row.update(state={name: getattr(counter, name) for name in fields}, native_active=native.active,
                       pending=len(native.pending), trace=native.trace)
            native.trace = []; output.append(copy.deepcopy(row))
        return output

    jobs = []
    def interval(start, end, events): return dict(start_event=start, end_event=end, commands=[dict(event=e) for e in events])
    a = interval(10, 30, [10, 20, 30]); b = interval(50, 60, [50, 60])
    ranges = [dict(a, range_index=8), dict(b, range_index=21)]
    def cmd(e, fail=False): return dict(op='command', event=e, fail=fail)
    verify = dict(op='verify'); flush = dict(op='flush')
    for op in ('interval', 'ranges'):
        for callbacks in (False, True):
            base = dict(op=op, interval=a, ranges=ranges, callbacks=callbacks)
            for sequence in ([1, 10, 20, 30, 40, 50, 60, 70], [10, 30], [20, 30], [10, 20],
                             [10, 20, 30, 30], [10, 20, 30, 50, 60, 60], [], [1, 2], [1, 1],
                             [10, 20, 30, 60], [10, 20, 30, 40, 50, 55, 60]):
                jobs.append(dict(base, steps=[verify] + [cmd(e) for e in sequence] + [verify, flush, verify]))
            for event in (1, 10, 20, 30, 40, 50, 60, 70):
                jobs.append(dict(base, steps=[cmd(e, e == event) for e in (1, 10, 20, 30, 40, 50, 60, 70)] + [verify, flush]))
            for fault in ('begin', 'end', 'submit', 'consume'):
                for clear in (False, True):
                    steps = [dict(op='fault', value=fault), cmd(10), cmd(20), cmd(30), flush, verify]
                    if clear: steps += [dict(op='fault', value=''), cmd(50), cmd(60), flush, verify]
                    jobs.append(dict(base, steps=steps))
            rng = random.Random(4532)
            for _ in range(100):
                sequence = [cmd(rng.choice((0, 1, 10, 20, 30, 40, 50, 60, 70))) for _ in range(rng.randrange(1, 15))]
                jobs.append(dict(base, steps=sequence + [flush, verify]))
    for callbacks in (False, True):
        jobs.append(dict(op='ranges', ranges=[], callbacks=callbacks, steps=[verify, cmd(1), cmd(2), verify]))
        jobs.append(dict(op='ranges', ranges=[dict(interval(10, 10, [10]), range_index=2)], callbacks=callbacks,
                         steps=[cmd(1), cmd(10), verify, flush, cmd(11), verify]))
    fixture = args.out / 'selection.gpa_frame'
    write_capture(fixture, [(1, 5, 0x99), (2, 7, 0x242), (5, 7, 0x35), (7, 7, 0xffff),
                           (10, 7, 0x39), (12, 7, 0x244), (15, 7, 0x32), (18, 7, 0x35)])
    empty = args.out / 'empty.gpa_frame'; write_capture(empty, [(1, 5, 0x99)])
    captures = [fixture, empty, args.reference / 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame',
                args.reference / 'bf1_2026_01_21__16_53_05.gpa_frame']
    selection_checks = 0
    for path in captures:
        with Frame(path) as frame:
            api = sorted(e.id for e in frame.entries.values() if e.category == 7)
            draws = [e.id for e in frame.entries.values() if e.category == 7 and 0x35 <= e.type <= 0x3d]
            endpoints = [None, True, -1, 999999999, 1.5]
            if api: endpoints += [api[0], api[len(api)//2], api[-1]]
            for start in endpoints:
                for end in endpoints:
                    jobs.append(dict(op='interval_selection', capture=str(path.resolve()), start=start, end=end, disabled=draws[:1]))
                    selection_checks += 1
            count = len(frame.metric_index()['ranges'][2])
            for indices in (None, [], [True], [-1], [0.0], [count], [0, 0], [0], [count - 1, 0], list(range(count))):
                jobs.append(dict(op='frame_selection', capture=str(path.resolve()), indices=indices, disabled=draws[:1]))
                selection_checks += 1
    expected = []
    for job in jobs:
        try: expected.append(reference(copy.deepcopy(job)))
        except Exception as exc: expected.append(dict(error=str(exc)))
    request, response = args.out / 'request.json', args.out / 'response.json'
    request.write_text(json.dumps(jobs), encoding='utf-8')
    env = os.environ.copy()
    if args.isolated_env:
        env = {k: v for k, v in env.items() if k.upper() in {'SYSTEMROOT', 'SYSTEMDRIVE', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
        windows = Path(os.environ['SystemRoot']); env['PATH'] = str(windows / 'System32') + os.pathsep + str(windows)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    subprocess.run([str(args.exe.resolve()), '--probe', str(request.resolve()), str(response.resolve())], env=env, check=True, timeout=180)
    actual = json.loads(response.read_text(encoding='utf-8')); assert len(actual) == len(expected)
    mismatches = [dict(index=i, job=jobs[i], expected=e, actual=a) for i, (e, a) in enumerate(zip(expected, actual)) if e != a]
    summary = dict(passed=not mismatches, cases=len(jobs), selection_checks=selection_checks,
        scope_observations=sum(len(j.get('steps', [])) for j in jobs),
        exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
        reference_sha256={n: hashlib.sha256((args.reference / 'standalone' / n).read_bytes()).hexdigest() for n in ('md_interval.py', 'md_frame_ranges.py')},
        mismatches=mismatches)
    (args.out / 'validation.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'mismatches'})); print('Mismatches:', len(mismatches))
    if mismatches: raise SystemExit(1)


if __name__ == '__main__': main()

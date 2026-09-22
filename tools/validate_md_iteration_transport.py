"""Compare native MD sessions with the original Python controller and publisher."""
import argparse
import copy
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import random
import subprocess
import sys
from types import SimpleNamespace


class Samples:
    supports_samples = supports_reuse = True

    def __init__(self, job):
        self.catalog = job['catalog']
        self.supports_reuse = job.get('supported', True)
        self.ready_after = job.get('ready_after', 0)
        self.cached = []
        self.owned = {}
        self.created = self.reused = self.serial = self.reads = 0
        self.trace = []
        self.error = ''
        self.handle = 1
        self.close_failures = job.get('close_failures', 0)
        self.lock = None

    def fail(self, name):
        if name in self.error.split(','):
            raise RuntimeError(name)

    def select(self, name):
        self.trace.append(['select', name]); self.fail('select')
        self.selected = next(s for s in self.catalog['sets'] if s['name'] == name)

    def clock_pair(self):
        self.trace.append(['clock_pair']); self.fail('clock')
        if any(v['state'] == 'begun' for v in self.owned.values()):
            raise RuntimeError('Clock during begun sample')
        i = self.reads; self.reads += 1
        return dict(maximum_ns=1000000, frequency_hz=1000000000, status=0,
                    gpu_ns=100 + i * 10, cpu_ns=1000 + i * 10)

    def sample_reserve(self, count):
        self.trace.append(['reserve', count]); self.fail('reserve')
        if self.owned or self.cached: raise RuntimeError('Invalid reserve')
        for _ in range(count):
            self.created += 1
            self.cached.append(dict(id=self.created, uses=0, polls=0, state='cached'))

    def sample_begin(self):
        self.trace.append(['begin']); self.fail('begin')
        if not self.cached: raise RuntimeError('Empty native cache')
        item = self.cached.pop(); self.reused += item['uses'] != 0
        item['uses'] += 1; item['polls'] = 0; item['state'] = 'begun'
        self.serial += 1; self.owned[self.serial] = item
        return self.serial

    def sample_info(self, token):
        self.trace.append(['info', token]); self.fail('info')
        return self.owned[token]['id']

    def sample_submit(self, token):
        self.trace.append(['submit', token]); self.fail('submit')
        item = self.owned[token]
        if item['state'] != 'begun': raise RuntimeError('Invalid submit')
        item['state'] = 'ended'

    def sample_poll(self, token, flush):
        self.trace.append(['poll', token, flush]); self.fail('poll')
        item = self.owned[token]
        if item['state'] == 'begun': raise RuntimeError('Invalid poll')
        item['polls'] += 1
        if flush or self.ready_after and item['polls'] >= self.ready_after: item['state'] = 'ready'
        if item['state'] == 'ready': return dict(available=token % 2 == 0), bytes([token % 256])

    def sample_recycle(self, token):
        self.trace.append(['recycle', token]); self.fail('recycle')
        item = self.owned[token]
        if item['state'] != 'ready': raise RuntimeError('Invalid recycle')
        del self.owned[token]; self.cached.append(item)

    def sample_release(self, token):
        self.trace.append(['release', token]); self.fail('release')
        if token not in self.owned: raise RuntimeError('Unknown release token')
        del self.owned[token]

    def sample_clear_cache(self):
        self.trace.append(['clear']); self.fail('clear')
        if self.owned: raise RuntimeError('Live native counters')
        self.cached.clear()

    def sample_stats(self):
        self.trace.append(['stats']); self.fail('stats')
        return dict(created=self.created, reused=self.reused, cached=len(self.cached), owned=len(self.owned))

    def sample_count(self): return len(self.owned)

    def close(self):
        self.trace.append(['close_device', self.lock.depth if self.lock else 0])
        if self.close_failures:
            self.close_failures -= 1
            raise RuntimeError('native close')
        self.handle = None; self.owned.clear(); self.cached.clear()


class Lock:
    def __init__(self, samples): self.samples = samples; self.depth = 0; self.closed = False
    def set_priority(self, value): self.samples.trace.append(['priority', value])
    def acquire(self): self.samples.trace.append(['acquire']); self.depth += 1; return True
    def release(self): self.samples.trace.append(['release_lock']); self.depth = max(0, self.depth - 1)
    def close(self):
        self.samples.trace.append(['close_lock', self.samples.handle is None, self.depth])
        self.depth = 0; self.closed = True
    def audit(self): return dict(depth=self.depth, closed=self.closed)


def session(job, module):
    from metric_iterations import MetricIterationRunner
    samples = Samples(job); fault = ''; iteration = 0
    def factory(catalog):
        samples.lock = Lock(samples)
        return samples.lock
    module.metric_device_mutex = factory
    def acquire(transport, pass_index, ranges):
        nonlocal iteration
        sample = iteration; iteration += 1
        for i, event in enumerate(ranges):
            if fault == 'omit' and i + 1 == len(ranges): continue
            def consume(result, raw, event=event):
                if fault == 'consumer': raise RuntimeError('consumer')
                values = []
                for metric in samples.selected['metrics']:
                    t = metric['result_type']; n = raw[0]
                    values.append(dict(type=t, value=(n % 2 == 0) if t == 3 else n + .25 if t == 2 else 1000 + n))
                transport.deliver(samples.selected, dict(set=samples.selected['name'], event=event,
                    pass_index=pass_index, sample_index=sample, raw_report='fixture.bin', raw_sha256='0' * 64,
                    available=job.get('all_available', False) or result['available'], unavailable_reasons=[],
                    values=values, information=[]))
            transport.begin(consume)
            if fault == 'acquire': raise RuntimeError('acquire')
            transport.submit()
    transport = module.MdIterationTransport(samples, SimpleNamespace(ptr=0x12345678), job['symbols'], acquire)
    output = dict(catalog=transport.catalog, plan=transport.plan, ids=list(transport.ids)); rows = []
    for step in job['steps']:
        row = {}; value = None
        try:
            op = step['op']
            if op == 'prepare':
                groups, flag = transport.prepare(step['ids']); value = dict(groups=groups, flag=flag)
            elif op == 'catalog': value = transport.descriptions()
            elif op == 'replay': value = transport.replay(step.get('pass', 0), step['ranges'], step.get('flag', False))
            elif op == 'collect': value = MetricIterationRunner(transport).execute(transport.ids, step['ranges'], **step.get('options', {}))
            elif op == 'fault': samples.error = step.get('driver', ''); fault = step.get('acquire', '')
            elif op == 'close': transport.close()
            row['return'] = value
        except Exception as exc: row['error'] = str(exc)
        row['closed'] = transport.closed
        try: row['audit'] = transport.audit()
        except Exception as exc: row['audit_error'] = str(exc)
        row['publisher'] = transport.publisher_values.report()
        row['trace'] = samples.trace; samples.trace = []
        rows.append(copy.deepcopy(row))
    samples.error = ''; samples.close_failures = 0; transport.close()
    output['steps'] = rows
    return output


def normalized(value):
    if isinstance(value, float) and not math.isfinite(value): return None
    if isinstance(value, dict): return {k: normalized(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)): return [normalized(v) for v in value]
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('reference', 'exe', 'qt-bin', 'out'): parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--isolated-env', action='store_true')
    args = parser.parse_args(); args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.reference / 'standalone'))
    import md_iteration_transport as reference
    def metric(name, type=1, kind=0):
        return dict(name=name, label=name, description=name, unit='ns' if name == 'GpuTime' else 'count',
                    result_type=type, metric_type=0, gpa_kind=kind)
    a, b, c, time, clocks = metric('A', 2, 3), metric('B'), metric('C', 3, 5), metric('GpuTime'), metric('GpuCoreClocks')
    catalog = dict(sets=[dict(name='First', metrics=[a, c, time, clocks], information=[]),
                         dict(name='Second', metrics=[b, c, time, clocks], information=[])])
    jobs = []
    for symbols in (['A'], ['B'], ['C'], ['A', 'B'], ['B', 'A'], ['GpuTime', 'A'], ['A', 'GpuCoreClocks']):
        for ready, count, available in itertools.product((0, 1, 3), (1, 4, 257), (False, True)):
            steps = [dict(op='catalog'), dict(op='prepare', ids=list(range(1, len(symbols) + 1)))]
            passes = len(reference.descriptors(catalog, symbols)[1]['passes'])
            for _ in range(2):
                # Explicit nonzero pass first exercises receiver reset without a preceding pass zero.
                steps += [dict(op='replay', **{'pass': p}, ranges=list(range(count))) for p in reversed(range(passes))]
            steps += [dict(op='close'), dict(op='close')]
            jobs.append(dict(op='iteration_session', catalog=catalog, symbols=symbols,
                             ready_after=ready, all_available=available, steps=steps))
    base = dict(op='iteration_session', catalog=catalog, symbols=['A', 'B'])
    for samples, selected, weighted in itertools.product((1, 3), (0, 1, 0xffffffff), (False, True)):
        options = dict(samples=samples, requested_pass=selected)
        if weighted: options['weights'] = [1., 2., 4.]
        jobs.append(dict(base, all_available=True, steps=[dict(op='collect', ranges=[2, 4, 9], options=options), dict(op='close')]))
    for driver, failure in [('', 'acquire'), ('release', 'acquire'), ('clear', 'acquire'), ('release,stats', 'acquire'),
                            ('select', ''), ('reserve', ''), ('begin', ''), ('info', ''), ('submit', ''),
                            ('poll', ''), ('recycle', ''), ('clock', ''), ('', 'consumer'), ('', 'omit')]:
        for retries in (0, 1, 3):
            jobs.append(dict(base, close_failures=retries, steps=[dict(op='prepare', ids=[1, 2]),
                dict(op='fault', driver=driver, acquire=failure), dict(op='replay', ranges=[11, 22, 33]),
                dict(op='close'), dict(op='fault'), dict(op='close'), dict(op='close')]))
    for ids in ([], [1, 1], [99], [True, 2.0], [2, 1], [4, 3, 1, 2]):
        jobs.append(dict(base, steps=[dict(op='prepare', ids=ids), dict(op='replay', ranges=[1, 2]), dict(op='close')]))
    for flags, ranges, index in ((True, [1], 0), (False, [], 0), (False, [1], 9)):
        jobs.append(dict(base, steps=[dict(op='prepare', ids=[1, 2]),
            dict(op='replay', flag=flags, ranges=ranges, **{'pass': index}), dict(op='close')]))
    rng = random.Random(4901)
    for _ in range(150):
        custom = copy.deepcopy(catalog)
        for s in custom['sets']: s['metrics'] = [m for m in s['metrics'] if rng.randrange(4)]
        symbols = rng.sample(['A', 'B', 'C', 'GpuTime', 'GpuCoreClocks'], rng.randrange(1, 6))
        jobs.append(dict(op='iteration_descriptors', catalog=custom, symbols=symbols))
    for symbols in ([], ['A', 'A'], ['Missing']): jobs.append(dict(base, symbols=symbols, steps=[]))
    jobs.append(dict(base, supported=False, steps=[]))
    expected = []
    for job in jobs:
        try:
            if job['op'] == 'iteration_descriptors':
                descriptors, plan = reference.descriptors(job['catalog'], job['symbols'])
                result = dict(catalog=descriptors, plan=plan)
            else: result = session(copy.deepcopy(job), reference)
            expected.append(normalized(result))
        except Exception as exc: expected.append(dict(error=str(exc)))
    request, response = args.out / 'request.json', args.out / 'response.json'
    request.write_text(json.dumps(jobs), encoding='utf-8')
    env = os.environ.copy()
    if args.isolated_env:
        env = {k: v for k, v in env.items() if k.upper() in {'SYSTEMROOT', 'SYSTEMDRIVE', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
        windows = Path(os.environ['SystemRoot']); env['PATH'] = str(windows / 'System32') + os.pathsep + str(windows)
    env['PATH'] = str(args.qt_bin.resolve()) + os.pathsep + env['PATH']
    subprocess.run([str(args.exe.resolve()), '--probe', str(request.resolve()), str(response.resolve())], env=env, check=True, timeout=180)
    actual = json.loads(response.read_text(encoding='utf-8'))
    assert len(actual) == len(expected)
    successful_replays = sum(isinstance(s.get('return'), dict) and bool(s['return'].get('metrics'))
                             for case in expected for s in case.get('steps', []))
    completed_iterations = sum(isinstance(s.get('return'), dict) and s['return'].get('complete') is True
                               for case in expected for s in case.get('steps', []))
    assert successful_replays >= 300 and completed_iterations == 12, (successful_replays, completed_iterations)
    mismatches = [dict(index=i, job=jobs[i], expected=e, actual=a) for i, (e, a) in enumerate(zip(expected, actual)) if e != a]
    summary = dict(passed=not mismatches, cases=len(jobs), sessions=sum(j['op'] == 'iteration_session' for j in jobs),
        observations=sum(len(j.get('steps', [])) for j in jobs), successful_replays=successful_replays,
        completed_iterations=completed_iterations, exe_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(), mismatches=mismatches)
    summary['reference_sha256'] = {name: hashlib.sha256((args.reference / 'standalone' / name).read_bytes()).hexdigest()
        for name in ('md_iteration_transport.py', 'md_scheduled_pool.py', 'md_publisher_values.py',
                     'metric_iterations.py', 'metric_pass_controller.py', 'metric_probe_registry.py')}
    (args.out / 'validation.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'mismatches'})); print('Mismatches:', len(mismatches))
    if mismatches: raise SystemExit(1)


if __name__ == '__main__': main()

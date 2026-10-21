"""Compare isolated and retained RDC requests; writes only a new evidence directory."""
import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--job', type=Path, required=True)
    parser.add_argument('--worker', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--library', type=Path)
    parser.add_argument('--resident', action='store_true')
    parser.add_argument('--count', type=int, default=20)
    parser.add_argument('--executions', help='Comma-separated execution identities, cycled in order')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    base = json.loads(args.job.read_text(encoding='utf-8-sig'))
    if args.library:
        base['renderdoc'] = str(args.library.resolve())
    env = os.environ.copy()
    qt = Path('D:/Qt/6.11.2/msvc2022_64/bin')
    if qt.exists(): env['PATH'] = str(qt) + os.pathsep + env.get('PATH', '')
    executions = args.executions.split(',') if args.executions else [base.get('execution')]
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    rows, process = [], None
    with (args.out / 'worker.log').open('w', encoding='utf-8') as log:
        try:
            if args.resident:
                process = subprocess.Popen([str(args.worker.resolve()), '--serve'], stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8', env=env, creationflags=flags)
            def send(command, serial, **extra):
                process.stdin.write(json.dumps(dict(schema=1, id=str(serial), generation='1', command=command, **extra)) + '\n')
                process.stdin.flush()
                line = process.stdout.readline()
                if not line: raise RuntimeError('Analysis service exited; inspect worker.log')
                reply = json.loads(line)
                if not reply.get('ok'): raise RuntimeError(str(reply))
                return reply
            started = time.perf_counter()
            if process:
                opening = dict(base, out=str((args.out / 'open').resolve()))
                reply = send('open', 1, job=opening)
                (args.out / 'open.json').write_text(json.dumps(reply, indent=2), encoding='utf-8')
            open_ms = (time.perf_counter() - started) * 1000
            for i in range(args.count):
                job = dict(base, out=str((args.out / f'request-{i}').resolve()))
                if executions[i % len(executions)] is not None:
                    job['execution'] = executions[i % len(executions)]
                job_path = args.out / f'job-{i}.json'
                job_path.write_text(json.dumps(job, indent=2), encoding='utf-8')
                started = time.perf_counter()
                if process:
                    reply = send('execute', i + 2, job=job)
                else:
                    subprocess.run([str(args.worker.resolve()), '--job', str(job_path.resolve())],
                        stdout=log, stderr=log, env=env, creationflags=flags, check=True, timeout=180)
                elapsed = (time.perf_counter() - started) * 1000
                result = json.loads((Path(job['out']) / 'result.json').read_text(encoding='utf-8-sig'))
                if not result.get('ok'): raise RuntimeError(str(result.get('error')))
                rows.append(dict(execution=job.get('execution'), elapsed_ms=elapsed, latency=result.get('latency')))
                print(f'{i + 1}/{args.count}: {elapsed:.1f} ms', flush=True)
            ordered = sorted(row['elapsed_ms'] for row in rows)
            summary = dict(mode='resident' if process else 'isolated', open_ms=open_ms, samples=rows,
                p50_ms=statistics.median(ordered), p95_ms=ordered[max(0, math.ceil(len(ordered)*0.95)-1)])
            (args.out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        finally:
            if process:
                process.stdin.close()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()


if __name__ == '__main__':
    main()

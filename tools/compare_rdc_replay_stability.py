"""Measure same-path replay stability without relaxing byte-equality checks.

Each input is a saved resource-workspace job. Between samples, navigate to a
different executed action so the next boundary read really replays GPU work.
An unstable input remains unverified for cross-path byte equality; this script
does not turn similar images into a correctness pass.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import queue
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rdc', required=True, type=Path)
    parser.add_argument('--prefix-job', required=True, type=Path)
    parser.add_argument('--reuse-job', required=True, type=Path)
    parser.add_argument('--navigate-execution', required=True)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--samples', type=int, default=3)
    args = parser.parse_args()
    if args.samples < 2 or args.samples > 10:
        parser.error('--samples must be between 2 and 10')
    args.out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    qt = Path('D:/Qt/6.11.2/msvc2022_64/bin')
    if qt.exists(): env['PATH'] = str(qt) + os.pathsep + env.get('PATH', '')
    rows = []
    for mode, source in [('prefix', args.prefix_job), ('reuse', args.reuse_job)]:
        job = json.loads(source.read_text(encoding='utf-8'))
        if job['action'] != 'resource-workspace' or job['execution'] == args.navigate_execution:
            raise ValueError('Use a resource-workspace job and a distinct navigation execution')
        with (args.out / f'{mode}.stderr.log').open('w', encoding='utf-8') as error:
            process = subprocess.Popen([str(args.rdc.resolve()), '--serve'], env=env,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error, text=True, encoding='utf-8',
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            serial = 0
            def send(command, request):
                nonlocal serial
                serial += 1
                process.stdin.write(json.dumps(dict(schema=1, id=str(serial), generation='1', command=command, job=request))+'\n')
                process.stdin.flush()
                response = queue.Queue()
                threading.Thread(target=lambda: response.put(process.stdout.readline()), daemon=True).start()
                reply = json.loads(response.get(timeout=240))
                if not reply.get('ok'): raise RuntimeError(reply)
            try:
                send('open', dict(job, out=str((args.out / f'{mode}-open').resolve())))
                for sample in range(args.samples):
                    output = args.out / f'{mode}-{sample}'
                    send('execute', dict(job, out=str(output.resolve())))
                    report = json.loads((output / 'result.json').read_text(encoding='utf-8'))
                    hashes = [item['sha256'] for item in report['resource_results']]
                    rows.append(dict(mode=mode, sample=sample, sha256=hashes))
                    print(json.dumps(rows[-1]), flush=True)
                    navigate = dict(job, action='pipeline', execution=args.navigate_execution,
                        out=str((args.out / f'{mode}-navigate-{sample}').resolve()))
                    navigate.pop('boundary', None); navigate.pop('resource_requests', None)
                    # Saved GUI identities bind the original selection, so do
                    # not reuse that envelope for the diagnostic navigation.
                    navigate.pop('request_identity', None)
                    send('execute', navigate)
            finally:
                process.stdin.close()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
    stability = {}
    for mode in ['prefix', 'reuse']:
        samples = [row['sha256'] for row in rows if row['mode'] == mode]
        stability[mode] = [len(set(values)) == 1 for values in zip(*samples)]
    summary = dict(samples=rows, stable_resources=stability,
        scope='Same-path byte stability only; unstable resources do not establish cross-path correctness.')
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()

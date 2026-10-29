"""Run original receiver-contract probes serially, outside product tests."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    captures = [('gf2', 'GF2_Exilium_2026_03_03__00_19_35.gpa_frame', 113),
                ('bf1', 'bf1_2026_01_21__16_53_05.gpa_frame', 876),
                ('helldivers', 'helldivers2_2026_04_02__18_02_58.gpa_frame', 200)]
    report = dict(schema=1, completed=False, original_engine_recovered=False, runs=[])
    manifest = args.out / 'runs.json'
    def save():
        manifest.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    save()
    for name, filename, event in captures:
        for vendor in (4318, 32902):
            for seed in (0, 0x13579bdf):
                output = (args.out / f'{name}-{vendor}-{seed}').resolve()
                command = [sys.executable, str(Path(__file__).with_name('probe_original_debugger_host.py')),
                           str((args.captures / filename).resolve()), '--event', str(event),
                           '--vendor', str(vendor), '--out', str(output), '--synthetic-engine-contract',
                           '--contract-selection-seed', str(seed), '--host-graph']
                row = dict(name=name, event=event, vendor=vendor, seed=seed, command=command,
                           report=str(output / 'dispatch.json'), completed=False)
                report['runs'].append(row)
                save()
                with (args.out / f'{output.name}.log').open('wb') as log:
                    try:
                        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=150)
                        row.update(exit_code=result.returncode, completed=result.returncode == 0)
                    except subprocess.TimeoutExpired:
                        row['timed_out'] = True
                save()
                if not row['completed']:
                    raise SystemExit('Probe failed; retained manifest identifies the incomplete case')
    report['completed'] = True
    save()
    print(str(manifest.resolve()))


if __name__ == '__main__':
    main()

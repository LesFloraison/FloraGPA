"""Development-only SO lifetime evidence on hardware and WARP, strictly serial."""
import argparse
import hashlib
import json
import os
from pathlib import Path

from validate_corpus import run


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ['exe', 'manifest', 'captures-root', 'out']:
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    exe, root = args.exe.resolve(strict=True), args.captures_root.resolve(strict=True)
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = json.loads(args.manifest.read_text())
    env = {k: v for k, v in os.environ.items() if k.upper() in
           {'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'LOCALAPPDATA', 'APPDATA'}}
    env['PATH'] = os.environ['WINDIR'] + '/System32;' + os.environ['WINDIR']
    summary = {'schema': 'FloraGPA SO lifetime checks 1', 'completed': False,
               'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
               'manifest_sha256': hashlib.sha256(args.manifest.read_bytes()).hexdigest(), 'cases': []}

    def save():
        (args.out / 'validation.json').write_text(json.dumps(summary, indent=2) + '\n')

    try:
        for case in manifest['cases']:
            capture = (root / case['path']).resolve(strict=True)
            require(capture.is_relative_to(root) and hashlib.sha256(capture.read_bytes()).hexdigest() == case['sha256'],
                    'Capture identity mismatch')
            mode, folder = case['mode'], args.out / case['id']
            folder.mkdir()
            row = {'id': case['id'], 'mode': mode, 'runs': []}
            summary['cases'].append(row)

            def invoke(tag, command, options=(), fail=False):
                out = folder / tag
                command_line = [exe, command, capture, '--out', out, *map(str, options)]
                result = run(command_line, folder / (tag + '.log'), 60, env)
                result['tag'] = tag
                result['command'] = list(map(str, command_line))
                row['runs'].append(result)
                save()
                require(not result['timed_out'] and result['exit_code'] == (1 if fail else 0),
                        f"{case['id']}/{tag}: unexpected process result")
                if not fail and command != 'commands':
                    report = json.loads((out / 'report.json').read_text())
                    require(report['completed'] and not report['reference_pixels_used'], 'Incomplete/native pixel guard')
                    result['report'] = report
                return out

            command_dir = invoke('commands', 'commands')
            commands = json.loads((command_dir / 'commands.json').read_text())['commands']
            setters = [c for c in commands if c['name'] == 'SOSetTargets']
            values = lambda c: {f['name']: f['value'] for f in c['fields']}
            target = values(setters[0])['buffers[0]']
            row['initial_so_bindings'] = setters[0]
            row['so_setter_events'] = [c['id'] for c in setters]
            if mode >= 5:
                copy = next(c for c in commands if c['name'] == 'CopyResource' and values(c)['source'] == target)
                oracle = (capture.parent / 'native/so.bin').read_bytes()
                require(len(oracle) == 64, 'SO oracle must cover the entire buffer')
                draw = next(c for c in commands if c['name'] == 'Draw')['id']
                row['so_draw_event'] = draw if mode >= 6 else None
                row['readback_copy_event'] = copy['id']
                row['target_resource'] = target
                row['staging_resource'] = values(copy)['destination']
            for driver in ['hardware', 'warp']:
                driver_args = ['--warp'] if driver == 'warp' else []
                for repeat in range(2):
                    tag = f'{driver}-{repeat + 1}'
                    replay_dir = invoke(tag, 'replay', driver_args, fail=mode < 5)
                    if mode < 5:
                        error = json.loads((folder / (tag + '.log')).read_text())
                        require(not error['completed'] and error['error'] ==
                                f"Event {setters[0]['id']} (SOSetTargets): Missing capture entry {target}",
                                'Missing SO descriptor unexpectedly accepted or different failure')
                        row['runs'][-1]['report'] = error
                        continue
                    report = row['runs'][-1]['report']
                    require(report['rgba_sha256'] == case['reference_rgba_sha256'], 'Final image mismatch')
                    history = report['stream_output_history']
                    require(len(history) == (1 if mode >= 6 else 0), 'SO execution count mismatch')
                    if mode >= 6:
                        require(history[0] == dict(event=str(draw), stream=0, vertices_per_primitive=1,
                                                  primitives_written='1', primitives_storage_needed='1'),
                                'SO written/storage query mismatch')
                    for suffix, resource, event, before in [
                        ('so', target, draw if mode >= 6 else copy['id'], False),
                        ('staging', values(copy)['destination'], copy['id'], False),
                    ] + ([('before-so', target, draw, True)] if mode >= 6 else []):
                        options = driver_args + ['--id', resource, '--event', event] + (['--before'] if before else [])
                        output = invoke(tag + '-' + suffix, 'buffer', options)
                        data = (output / 'buffer.bin').read_bytes()
                        row['runs'][-1]['buffer_sha256'] = hashlib.sha256(data).hexdigest()
                        if not before:
                            require(data == oracle, 'SO cursor/storage oracle mismatch')
                        else:
                            row['runs'][-1]['already_equals_final'] = data == oracle
                    if mode >= 6:
                        invoke(tag + '-disabled-so', 'replay', driver_args + ['--disable', draw])
                        disabled = row['runs'][-1]['report']
                        require(disabled['stream_output_history'] == [], 'Disabled SO draw was executed')
                        require(disabled['rgba_sha256'] == case['reference_rgba_sha256'], 'Final marker control changed')
                save()
            row['passed'] = True
            save()
            print(case['id'], 'verified', flush=True)
        summary['completed'] = True
    finally:
        save()


if __name__ == '__main__':
    main()

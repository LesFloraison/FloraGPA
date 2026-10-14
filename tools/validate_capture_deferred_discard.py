"""Serial dual-player checks for deferred discards or explicit counters; no GF2/GUI acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('probe', 'replay', 'oracle-tools', 'qt-bin', 'out'):
        parser.add_argument('--'+key, type=Path, required=True)
    parser.add_argument('--profile', choices=('discard', 'counters', 'om-counters', 'initial-counters', 'initial-om-counters'), default='discard')
    args = parser.parse_args()
    counter_profile = args.profile != 'discard'
    om_profile = args.profile.endswith('om-counters')
    initial_profile = args.profile.startswith('initial-')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env.pop('GPA_LOCAL_INJECT', None)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env['PATH'] = str(args.qt_bin.resolve())+os.pathsep+env['PATH']
    checks = []

    def check(name, passed):
        checks.append(dict(name=name, passed=bool(passed)))
        (out/'validation.json').write_text(json.dumps(dict(
            scope=__doc__+'; profile='+args.profile, passed=all(x['passed'] for x in checks), checks=checks), indent=2)+'\n')
        if not passed:
            raise AssertionError(name)

    def run(name, command):
        result = subprocess.run([str(x) for x in command], env=env, capture_output=True, timeout=120)
        (out/(name+'.log')).write_bytes(result.stdout+b'\n'+result.stderr)
        check(name, result.returncode == 0)
        return result.stdout

    modes = {'aba': ((255, 0, 0, 255), 1, 3),
             'bab': ((0, 255, 0, 255), 1, 3),
             'missing-discard': ((0, 255, 0, 255), 1, 3),
             'missing-draw': ((0, 0, 0, 255), 0, 3)}
    if counter_profile:
        modes = {}
        for kind in ('counter', 'append'):
            for frame in range(2):
                prefix = f'{kind}-{frame}'
                modes[prefix] = ((0, 255, 0, 255) if frame else (255, 0, 0, 255), 1, 6 if frame else 3)
                modes[prefix+'-reset-again'] = ((0, 0, 255, 255), 1, 6 if frame else 3)
                modes[prefix+'-no-draw'] = ((0, 0, 0, 255), 0, 6 if frame else 3)
        if om_profile:
            modes = {key: (color, draws+dispatches, 0) for key, (color, draws, dispatches) in modes.items()}
    for device in ('warp', 'hardware'):
        probe = run(device+'-probe', [args.probe.resolve(),
                    *(['--hardware'] if device == 'hardware' else []), out/device])
        check(device+'-source-objects-dead', b'source objects alive=0' in probe)
        for mode, (color, draws, dispatches) in modes.items():
            frame = out/device/(mode+'.gpa_frame')
            name = device+'-'+mode
            expected = bytes(color)*64
            run(name+'-flora', [args.replay.resolve(), 'replay', frame, '--out', out/(name+'-flora')])
            run(name+'-original', [sys.executable, args.oracle_tools.resolve()/'replay_frame.py', frame,
                                  '--out', out/(name+'-original'), '--timeout', '60'])
            flora = json.loads((out/(name+'-flora/report.json')).read_text())
            original = json.loads((out/(name+'-original/replay_report.json')).read_text())
            check(name+'-both-exact-cpu', (out/(name+'-flora/frame.rgba')).read_bytes() == expected and
                  original['replayed_rgba_sha256'] == hashlib.sha256(expected).hexdigest())
            check(name+'-native-execution', flora['reference_pixels_used'] is False and
                  original['native']['open_status'] == 0 and original['native']['playback_status'] == 0 and
                  original['native']['closed'] and original['new_framebuffer_count'] > 0)
            check(name+'-captured-reference-control', original['comparison_to_captured_resource']['rgba_equal'] is
                  (not (mode.startswith('missing-') or mode.endswith(('-reset-again', '-no-draw')))))
            maps = 2 if mode == 'missing-discard' else 3
            counts = dict(Dispatch=dispatches, Present=1, Map=maps, unmap_observations=maps)
            if counter_profile:
                counts = dict(Present=1)
                actions = 6 if mode.split('-')[1] == '1' else 3
                resets = actions if mode.endswith('-reset-again') else actions//3*2
                if not om_profile:
                    counts['Dispatch'] = dispatches
                else:
                    counts.update(ClearState=resets, annotation_records=2*resets)
            if draws:
                counts['Draw'] = draws
            check(name+'-actual-events', flora['counts'] == counts)
            run(name+'-inventory', [args.replay.resolve(), 'inventory', frame, '--out', out/(name+'-inventory')])
            inventory = json.loads((out/(name+'-inventory/report.json')).read_text())
            check(name+'-inventory', inventory['draws'] == draws and inventory['dispatches'] == dispatches)
            if counter_profile:
                records = [c for c in inventory['commands'] if c['type'] == 0x25e]
                check(name+'-explicit-counter-records', len(records) == resets*(2 if om_profile else 1))
                resources = [r for r in inventory['resources'] if r['type'] == 0x83]
                if om_profile:
                    commands = [c for c in inventory['commands'] if c['type'] == 0x37]
                    last_dispatch = commands[-1 if mode.endswith('-no-draw') else -2]['id']
                else:
                    last_dispatch = [c for c in inventory['commands'] if c['type'] == 0x35][-1]['id']
                run(name+'-buffer', [args.replay.resolve(), 'buffer', frame, '--id', resources[0]['id'],
                                    '--event', last_dispatch, '--out', out/(name+'-buffer')])
                first, last = (4, 1) if mode.split('-')[1] == '1' else (2, 5)
                indices = {first, last}
                if not mode.endswith('-reset-again'):
                    indices.add(first+1)
                if initial_profile and mode.split('-')[1] == '1':
                    indices.add(last+1)
                    if not mode.endswith('-reset-again'):
                        indices.add(last+2)
                expected_buffer = b''.join((17 if i in indices else 0).to_bytes(4, 'little') for i in range(16))
                check(name+'-flora-full-resource', len(resources) == (2 if om_profile else 1) and
                      (out/(name+'-buffer/buffer.bin')).read_bytes() == expected_buffer)
                counters = json.loads((out/(name+'-buffer/report.json')).read_text())['uav_counters']
                check(name+'-flora-final-counter', len(counters) == 1 and counters[0]['value'] == last+1)
            shaders = [r for r in inventory['resources'] if 0x90 <= r['type'] <= 0x95]
            check(name+'-three-shaders', len(shaders) == 3)
            for shader in shaders:
                prefix = name+'-shader-'+shader['id']
                run(prefix, [args.replay.resolve(), 'shader', frame, '--id', shader['id'], '--out', out/prefix])
                check(prefix+'-dxbc-assembly', (out/prefix/'shader.dxbc').read_bytes().startswith(b'DXBC') and
                      (out/prefix/'shader.asm').stat().st_size > 50)
            check(name+'-flora-independent-modules', not any('intelswtools/gpa/' in m.replace('\\', '/').lower()
                  or Path(m).name.lower().startswith(('python', 'qt5')) for m in flora['loaded_modules']))
    inputs = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
              (args.probe, args.replay, args.oracle_tools/'replay_frame.py')}
    inputs['frames'] = {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in out.rglob('*.gpa_frame')}
    (out/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    print(json.dumps(dict(passed=True, files=len(inputs['frames']), checks=len(checks))))


if __name__ == '__main__':
    main()

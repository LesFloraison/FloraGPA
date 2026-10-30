"""Check isolated private-history count artifacts, context, provenance and original references."""
import argparse
import copy
import itertools
import json
from pathlib import Path
import re
import struct
from validate_native_draw_history_jobs import read, validate, require
from validate_pixel_return_count import sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--prior-counts", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    modes = ("storage", "shader", "merger", "identity", "presence", "tests", "count", "all-msaa")
    expected = {f"{v}-{m}" for v, m in itertools.product((0, 4318, 32902), modes)}
    expected |= {f"boundary-{v}-{m}" for v, m in itertools.product((0, 4318, 32902), ("array", "depth", "no-ps", "depth-only", "depth-null", "depth-layer", "depth-mixed-null"))}
    expected |= {f"integer-{v}-{a}" for v, a in itertools.product((0, 4318, 32902), (1, 0x3f800000))}
    require({p.name for p in (args.root / "owned").iterdir()} == expected, "Incomplete owned matrix")
    controls = observed = inapplicable = 0
    cases = []
    for path in sorted((args.root / "owned").iterdir()):
        vendor = int(path.name.split('-')[1 if path.name.startswith(('boundary-', 'integer-')) else 0])
        job, result = read(path)
        require(job['backend'] == 'dx11-native/17', 'Backend identity')
        data = validate(job, result, vendor)
        count = data['return_count_observation']
        if path.name.startswith('integer-'):
            proof = json.loads((path / 'verified.json').read_text(encoding='utf-8'))
            expected_count = 3 if struct.unpack('<I', bytes(proof['reference_bytes'][:4]))[0] == 7 else 0
            require(proof['expected_count'] == expected_count == count['count']
                    and count['status'] == 'observed', 'Original integer reference')
            require(data['original_draw_observation']['after']['bytes'] == proof['reference_bytes']
                    and data['frame_sha256'] == sha(path / 'frame.gpa_frame'), 'Integer source evidence')
            require(count['observation']['count_transport']['auxiliary_target'] is True, 'Integer RT0 route')
        elif path.name.startswith('boundary-'):
            mode = path.name.split('-', 2)[2]
            require(data['frame_sha256'] == sha(path / 'frame.gpa_frame'), 'Boundary source evidence')
            if mode == 'depth-mixed-null':
                require(count['status'] == 'observed' and count['count'] == 1
                        and count['observation']['original_null_pixel_shader'] is True
                        and count['observation']['backend'] == 'native-depth-return-count/3', 'Mixed null PS count')
            elif mode.startswith('depth-'):
                require(count['count'] == 1 and count['status'] == 'observed', 'Depth Worker count')
                require(count['observation']['original_null_pixel_shader'] == (mode == 'depth-null'), 'Null PS identity')
                require(count['observation']['depth_transport']['source_mip'] == (1 if mode == 'depth-layer' else 0)
                        and count['observation']['depth_transport']['source_first_layer'] == (1 if mode == 'depth-layer' else 0), 'Depth subresource')
            elif mode == 'depth':
                require(count['count'] == 1 and count['status'] == 'observed'
                        and count['observation']['backend'] == 'native-depth-return-count/2', 'Mixed depth count')
            elif mode == 'array':
                require(count['count'] == 1 and count['status'] == 'observed'
                        and data['selection']['mip'] == data['selection']['layer'] == 1
                        and count['observation']['count_transport']['layer'] == 0, 'Absolute/relative subresource')
            else:
                require(count['status'] == 'not_applicable' and count['count'] is None, 'Inapplicable selection')
        else:
            mode = path.name.split('-', 1)[1]
            require(count['requested'] is (mode in ('count', 'all-msaa'))
                    and count['count'] == (3 if count['requested'] else None), 'Owned count mode')
            require(data['before']['bytes'] == list(struct.pack('<4f', .125, .5, .25, .25))
                    and data['after']['bytes'] == list(struct.pack('<4f', .25, .75, .125, .5)), 'Existing storage oracle')
        observed += count['status'] == 'observed'
        inapplicable += count['status'] == 'not_applicable'
        mutations = [lambda r: r.update(context_id='stale'),
                     lambda r: r['data']['return_count_observation'].update(requested=not count['requested']),
                     lambda r: r['data']['return_count_observation'].update(count=1 if count['count'] is None else count['count'] + 1),
                     lambda r: r['data']['return_count_observation'].update(original_fragment_eligibility=True),
                     lambda r: r['execution']['loaded_modules'].append('C:/unexpected/renderdoc.dll')]
        if count['status'] == 'observed':
            mutations += [lambda r: r['data']['return_count_observation']['observation']['depth_transport' if 'depth_transport' in count['observation'] else 'count_transport'].update(layer=99),
                          lambda r: r['data']['return_count_observation'].update(status='unknown')]
        for mutate in mutations:
            wrong = copy.deepcopy(result)
            mutate(wrong)
            try:
                validate(job, wrong, vendor)
            except (AssertionError, KeyError, ValueError):
                controls += 1
            else:
                raise AssertionError('Corrupted count artifact was accepted')
        cases.append({'case': path.name, 'status': count['status'], 'count': count['count']})
    real = []
    for name, vendor in itertools.product(('gf2', 'bf1', 'helldivers'), (0, 4318, 32902)):
        path = args.root / 'real' / f'{name}-{vendor}'
        job, result = read(path)
        data = validate(job, result, vendor)
        prior = json.loads((args.prior_counts / f'{name}-{vendor}' / 'report.json').read_text(encoding='utf-8'))
        count = data['return_count_observation']
        require(count['status'] == 'observed' and count['count'] == prior['return_count']['count']
                and data['frame_sha256'] == prior['frame_sha256'] and data['event'] == prior['event']
                and data['adapter'] == prior['adapter'], 'Independent native count comparison')
        real.append({'capture': name, 'vendor': vendor, 'count': count['count'],
                     'elapsed_ms': result['execution']['elapsed_ms'],
                     'peak_working_set_bytes': result['execution']['peak_working_set_bytes']})
    exits = json.loads((args.root / 'exits.json').read_text(encoding='utf-8'))
    require(len(exits) == 12 and all(row['exit_code'] == 0 for row in exits), 'Failed or missing process')
    totals = []
    for run, expected_passes in zip(exits[:3], (56, 61, 50)):
        match = re.search(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted',
                          (args.root / (run['label'] + '.txt')).read_text(encoding='utf-8'))
        require(match is not None and tuple(map(int, match.groups())) == (expected_passes, 0, 0, 0), 'Regression total')
        totals.append({'suite': run['label'], 'passed': expected_passes})
    summary = {'schema': 1, 'owned_jobs': len(cases), 'observed_owned_counts': observed,
               'inapplicable_owned_counts': inapplicable, 'corruptions_rejected': controls,
               'qt_totals': totals, 'real_controls': real, 'cases': cases, 'full_fragment_history_complete': False}
    args.out.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'cases'}, indent=2))


if __name__ == '__main__':
    main()

"""Check per-sample private merger test observations against their inputs."""
import argparse
import copy
import hashlib
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(result, outputs):
    require(result['kind'] == 'native_pixel_merger' and result['backend'] == 'native-pixel-merger/4' and
            result['original_fragment_eligibility'] is None and not result['original_early_depth_reconstructed'],
            'Wrong test scope')
    tests = result['test_observations']
    samples = result['sample_count']
    lane = outputs['lanes'][result['lane']]
    depth = result['depth_stencil_descriptor']
    require(tests['schema'] == 1 and tests['scope'] == 'private_late_tests_at_initial_storage' and
            tests['target_writes'] is False and tests['draws'] == samples * 4, 'Wrong diagnostic contract')
    require(tests['depth_enabled'] == bool(result['depth_format'] and depth[0]) and
            tests['stencil_enabled'] == bool(result['depth_format'] and depth[3]) and
            tests['alpha_to_coverage_enabled'] == bool(result['blend_descriptor'][0]), 'Wrong enabled tests')
    for key in ('shader_active', 'initially_covered', 'shader_discarded'):
        require(tests[key] == lane['discarded' if key == 'shader_discarded' else key], 'Wrong lane state')
    coverage = [field for field in lane['outputs'] if field['kind'] == 'coverage']
    mask = coverage[0]['raw_bits'][0] if coverage else 0xffffffff
    require(tests['shader_coverage_mask'] == mask, 'Wrong shader coverage')
    require([s['sample'] for s in tests['samples']] == list(range(samples)), 'Missing/reordered samples')
    total = 0
    for row in tests['samples']:
        sample = row['sample']
        for key, source in (('raster_covered', result['coverage_mask']),
                            ('sample_enabled', result['sample_mask']), ('shader_covered', mask)):
            require(row[key] == bool(source & (1 << sample)), 'Wrong coverage identity')
        for name in ('depth', 'stencil', 'alpha_to_coverage', 'combined'):
            observation = row[name]
            require(type(observation['passing_samples']) is int and observation['passing_samples'] in (0, 1)
                    and type(observation['passed']) is bool and
                    observation['passed'] == bool(observation['passing_samples']), 'Invalid sample query')
        for name, enabled in (('depth', tests['depth_enabled']), ('stencil', tests['stencil_enabled']),
                               ('alpha_to_coverage', tests['alpha_to_coverage_enabled'])):
            if not enabled:
                require(row[name]['passed'], 'Disabled independent test rejected a sample')
        # SV_Coverage suppresses alpha-to-coverage in the combined shader. The
        # independent alpha probe deliberately has no SV_Coverage output.
        expected = (lane['shader_active'] and row['raster_covered'] and row['sample_enabled'] and
                    row['shader_covered'] and row['depth']['passed'] and row['stencil']['passed'] and
                    (bool(coverage) or row['alpha_to_coverage']['passed']))
        require(row['combined']['passed'] == expected, 'Independent tests do not explain the combined result')
        total += row['combined']['passing_samples']
    require(total == result['passing_samples'], 'Sample queries differ from whole merger query')
    if 'reference_storage_unchanged' in result:
        require(result['reference_storage_unchanged'] is True and result['color_after'] == result['reference_color']
                and result['depth_after'] == result['reference_depth'], 'Original draw comparison failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--history', action='store_true', help='Validate joined draw-history observations')
    args = parser.parse_args()
    cases = samples = rejected = 0
    files = []
    for path in sorted(args.directory.rglob('*.json')):
        document = json.loads(path.read_text(encoding='utf-8'))
        entries = []
        if args.history:
            for row in document.get('records', []):
                for joined in row.get('private_output_mergers', []):
                    entries.append((joined['merger'], row['private_shader_outputs'][joined['shader_output_index']]))
        elif document.get('result', {}).get('kind') == 'native_pixel_merger':
            entries.append((document['result'], document['quad_outputs']))
        for result, outputs in entries:
            validate(result, outputs)
            cases += 1
            samples += result['sample_count']
            for key, value in (('target_writes', True), ('draws', 0), ('shader_coverage_mask', -1),
                               ('samples', []), ('scope', 'original_fragment')):
                bad = copy.deepcopy(result)
                bad['test_observations'][key] = value
                try:
                    validate(bad, outputs)
                except ValueError:
                    rejected += 1
                else:
                    raise ValueError('Corrupted test observation accepted')
        if entries:
            files.append(dict(path=path.as_posix(), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    require(cases > 0, 'No merger test evidence')
    args.out.write_text(json.dumps(dict(schema=1, scope='native_private_merger_test_observations', cases=cases,
        samples=samples, negative_controls=rejected, files=files), indent=2) + '\n', encoding='utf-8')
    print(f'{cases} merger cases, {samples} sample observations, {rejected} corruptions rejected')


if __name__ == '__main__':
    main()

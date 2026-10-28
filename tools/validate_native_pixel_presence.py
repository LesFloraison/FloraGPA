"""Verify accumulated primitive predicates, exact storage and scope boundaries."""
import argparse
import copy
import json
import struct
from pathlib import Path
from validate_native_pixel_completion import require
from validate_native_pixel_identity import predicate
from validate_native_pixel_primitive_mask import shader
from validate_native_pixel_geometry_replay import FRAME_HASHES

MODES = ('overlap', 'discard-first', 'discard-middle', 'discard-last', 'early-discard',
         'sample', 'sample-mask', 'alpha-zero', 'write-mask', 'mrt-eight', 'unorm', 'srgb', 'depth', 'stencil', 'depth-stencil', 'duplicate-gs')


TEST_MODES = ('disable_depth', 'disable_stencil', 'disable_depth_stencil', 'disable_alpha_to_coverage', 'all_samples')
TEST_FIXTURES = ('overlap', 'discard-middle', 'early-discard', 'depth', 'stencil', 'depth-stencil', 'sample-mask', 'alpha-zero')


def pipeline_test(row, mode):
    state = row['pipeline_test_state']
    require(state['mode'] == mode and state['scope'] == 'complete_private_draw_counterfactual' and
            state['failure_reason'] is None and isinstance(state['depth_view_bound'], bool), 'Counterfactual scope')
    expected = copy.deepcopy(state['depth_stencil_before'])
    if mode in ('disable_depth', 'disable_depth_stencil'):
        expected['depth_enabled'] = False
    if mode in ('disable_stencil', 'disable_depth_stencil'):
        expected['stencil_enabled'] = False
    require(state['depth_stencil_after'] == expected, 'Only the requested depth/stencil enables change')
    require(state['sample_mask_after'] == (0xffffffff if mode == 'all_samples' else state['sample_mask_before']) and
            state['alpha_to_coverage_after'] is (False if mode == 'disable_alpha_to_coverage' else state['alpha_to_coverage_before']),
            'Only the requested coverage state changes')
    require(row['completion_target']['sample_mask'] == state['sample_mask_after'] and
            row['completion_target']['alpha_to_coverage'] is state['alpha_to_coverage_after'], 'Effective coverage state')


def common(row, vendor, mode='preserve'):
    require(row['schema'] == 1 and row['kind'] == 'native_pixel_primitive_presence_observation' and
            row['backend'] == 'native-pixel-primitive-presence/1' and
            row['scope'] == ('original_ps_body_with_private_primitive_presence' if mode == 'preserve' else
                             'original_ps_body_with_counterfactual_primitive_presence'), 'Observation contract')
    require(row['original_fragment_eligibility'] is None and row['original_gpu_schedule'] is False and
            row['diagnostic_shader_replacements'] is True and row['extra_uav_slots'] == 0 and
            row['count_domain'] == 'entire_bound_output' and row['enabled'] is True, 'Unproved scope')
    if mode != 'preserve' or 'pipeline_test_state' in row:
        pipeline_test(row, mode)
    target = row['completion_target']
    require(target['cleared_to_zero'] is True and target['rgb_reduction'] == 'maximum' and
            target['alpha_reduction'] == 'replace' and target['logic_disabled'] is True and
            target['blend_and_logic_disabled'] is False and target['write_mask'] == 15, 'Reduction state')
    code = row['completion_shader']
    require(code['kind'] == 'native_pixel_primitive_presence_shader' and
            code['backend'] == 'native-pixel-primitive-presence-shader/1' and code['component_type'] == 3 and
            code['requires_private_unblended_target'] is False and code['requires_private_max_rgb_target'] is True and
            code['requires_zero_initial_rgb'] is True and code['requires_alpha_replace'] is True and
            code['alpha_preserved'] is True and code['nonmatching_rgb_zero'] is True, 'Shader transport contract')
    require(code['primitive_mask'] == 0xffffffff and code['target'] == target['slot'], 'Predicate selection')
    require(row['adapter']['vendor'] == (vendor or 5140) and row['adapter']['warp'] is (vendor == 0), 'Adapter')


def owned(row):
    mode, vendor = row['mode'], row['vendor']
    require(mode in MODES and vendor in (0, 4318, 32902), 'Owned mode')
    require(row['original_storage_restored'] is True and row['initial_storage'] == row['restored_storage'] and
            row['initial_depth_stencil'] == row['restored_depth_stencil'], 'Original restoration')
    samples = 4 if mode in ('sample', 'sample-mask', 'alpha-zero') else 1
    require(len(row['initial_storage']) == samples and len(row['passes']) == 4, 'Complete owned selection')
    for identity, item in enumerate(row['passes']):
        obs = item['observation']
        common(obs, vendor)
        system_id = 7 if mode == 'duplicate-gs' and identity == 0 else identity
        require(item['identity'] == obs['completion_shader']['primitive_value'] == system_id, 'Selected system ID')
        require(obs['parameters']['vertex_count'] == 9 and obs['event'] == 100, 'Original complete draw')
        require(item['other_targets'] == row['reference_other_targets'] and
                item['depth_stencil'] == row['reference_depth_stencil'], 'Independent output agreement')
        require(len(item['before']) == len(item['after']) == samples, 'Complete sample coverage')
        for sample, (before, after) in enumerate(zip(item['before'], item['after'])):
            expected = ((system_id == 7 if mode == 'duplicate-gs' else identity < 3) and not (mode == 'discard-first' and identity == 0) and
                        not (mode in ('discard-middle', 'early-discard') and identity == 1) and
                        not (mode == 'discard-last' and identity == 2) and
                        not (mode == 'sample' and identity == 1 and sample == 2) and
                        not (mode == 'sample-mask' and sample != 1) and mode != 'alpha-zero' and
                        not (mode in ('depth', 'stencil', 'depth-stencil') and identity != 0))
            require(predicate(before) is False and predicate(after) is expected, 'Authored primitive presence')
        variants = item.get('test_variants', [])
        if variants:
            require(mode in TEST_FIXTURES and len(variants) == 5, 'Owned counterfactual matrix')
            for test_mode, test in zip(TEST_MODES, variants):
                common(test['observation'], vendor, test_mode)
                require(test['observation']['shaders'] == obs['shaders'] and
                        test['observation']['parameters'] == obs['parameters'] and
                        test['observation']['raster_state'] == obs['raster_state'] and
                        test['observation']['completion_shader'] == obs['completion_shader'], 'Original executable and draw retained')
                before_state = obs['pipeline_test_state']
                after_state = test['observation']['pipeline_test_state']
                for key in ('depth_stencil_before', 'sample_mask_before', 'alpha_to_coverage_before', 'stencil_reference', 'depth_view_bound'):
                    require(before_state[key] == after_state[key], 'Fresh captured test state')
                require(len(test['before']) == len(test['after']) == samples, 'All counterfactual samples')
                for sample, (initial, final) in enumerate(zip(test['before'], test['after'])):
                    expected = (identity < 3 and
                        not (mode in ('discard-middle', 'early-discard') and identity == 1) and
                        not (mode in ('depth', 'depth-stencil') and identity != 0 and test_mode not in ('disable_depth', 'disable_depth_stencil')) and
                        not (mode in ('stencil', 'depth-stencil') and identity != 0 and test_mode not in ('disable_stencil', 'disable_depth_stencil')) and
                        not (mode == 'sample-mask' and sample != 1 and test_mode != 'all_samples') and
                        not (mode == 'alpha-zero' and test_mode != 'disable_alpha_to_coverage'))
                    require(predicate(initial) is False and predicate(final) is expected, 'Authored counterfactual result')
                if mode in ('depth', 'stencil', 'depth-stencil'):
                    require(test['depth_stencil'] == row['reference_counterfactual_depth'][test_mode], 'Independent counterfactual depth/stencil evolution')
    if row.get('history') is not None:
        from validate_native_pixel_draw_history import validate
        validate(row['history'], vendor)


def history_presence(history):
    root = history['primitive_presence_observation']
    require(root['requested'] is True and root['scope'] == 'system_id_color_completion_in_repeated_private_draws' and
            root['original_fragment_eligibility'] is None, 'History presence scope')
    if root['status'] == 'not_applicable':
        require(root['reason'] in ('selected_depth_target', 'no_pixel_shader') and root['observations'] == [],
                'Unavailable color presence')
        return 0
    require(root['status'] == 'observed' and root['pass_count'] == len(root['observations']) * (12 if root.get('test_variants_requested', False) else 2),
            'Presence repetitions')
    ids, references = set(), set()
    vendor = 0 if history['adapter']['warp'] else history['adapter']['vendor']
    for index, item in enumerate(root['observations']):
        identity = item['system_primitive_id']
        require(identity not in ids and len(item['passes']) == 2, 'Distinct system IDs')
        ids.add(identity)
        matches = []
        for row in history['records']:
            if row['original_primitive_id'] == identity:
                require(row['system_id_presence_observation'] == index and row['original_fragment_eligibility'] is None,
                        'System ID association remains unqualified')
                matches.append(row['primitive_ordinal'])
                references.add(row['primitive_ordinal'])
        require(matches == item['matching_primitive_ordinals'] and len(matches) > 0, 'Complete ambiguous ID association')
        values = []
        for value in item['passes']:
            obs = value['observation']
            common(obs, vendor)
            require(obs['frame_sha256'] == history['frame_sha256'] and obs['event'] == history['event'] and
                    obs['adapter'] == history['adapter'] and
                    obs['shaders'] == history['original_draw_observation']['shaders'] and
                    obs['completion_shader']['primitive_value'] == identity, 'Presence history context')
            require(predicate(value['before']) is False, 'Zero initial history RGB')
            values.append(predicate(value['after']))
        agreed = values[0] is not None and values[0] == values[1]
        require(item['status'] == ('repeat_agreement' if agreed else 'unverified') and
                item['color_completion_observed'] is (values[0] if agreed else None), 'Repeated observations')
        variants = item.get('test_variants', [])
        require(len(variants) == (5 if root.get('test_variants_requested', False) else 0), 'Requested test variants')
        for mode, variant in zip(TEST_MODES, variants):
            require(variant['mode'] == mode and variant['failure_reason'] is None and len(variant['passes']) == 2,
                    'Counterfactual repetitions and unknown attribution')
            observed = []
            for value in variant['passes']:
                obs = value['observation']
                common(obs, vendor, mode)
                baseline = item['passes'][0]['observation']
                for key in ('frame_sha256', 'event', 'adapter', 'shaders', 'parameters', 'raster_state', 'completion_shader'):
                    require(obs[key] == baseline[key], 'Counterfactual context: ' + key)
                for key in ('depth_stencil_before', 'sample_mask_before', 'alpha_to_coverage_before', 'stencil_reference', 'depth_view_bound'):
                    require(obs['pipeline_test_state'][key] == baseline['pipeline_test_state'][key], 'Counterfactual initial state: ' + key)
                require(predicate(value['before']) is False, 'Counterfactual zero clear')
                require(value['after']['typed_format'] == history['selection']['subresource']['format'], 'Counterfactual typed selection')
                observed.append(predicate(value['after']))
            agreement = observed[0] is not None and observed[0] == observed[1]
            expected = observed[0] if agreement else None
            require(variant['status'] == ('repeat_agreement' if agreement else 'unverified') and
                    variant['color_completion_observed'] is expected, 'Counterfactual agreement')
            base = item['color_completion_observed']
            require(variant['differs_from_preserved_tests'] is (None if base is None or expected is None else base != expected),
                    'Counterfactual comparison is not a failure cause')
    for row in history['records']:
        require((row['primitive_ordinal'] in references) is (row['original_primitive_id'] is not None),
                'All known candidate IDs observed')
        if row['original_primitive_id'] is None:
            require(row['system_id_presence_observation'] is None, 'Unknown identity remains unknown')
    return root['pass_count'] * 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--with-tests', action='store_true', help='Require complete output-test counterfactual evidence')
    parser.add_argument('--real', type=Path)
    parser.add_argument('--history-real', type=Path, help='Also verify complete CLI jobs against the ordinary-replay probes')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.directory.glob('*.json'))
    require({p.stem for p in paths} == {f'{vendor}-{mode}' for vendor in (0, 4318, 32902)
                                      for mode in (*(MODES if args.with_tests else tuple(m for m in MODES if m != 'depth-stencil')), 'recovery')}, 'Complete owned matrix')
    controls, count, real = 0, 0, []
    for path in paths:
        row = json.loads(path.read_text(encoding='utf-8'))
        if row['mode'] == 'recovery':
            if args.with_tests:
                require(row['override_failures_restored'] == 10 and row['invalid_override_rejected'] is True, 'Override recovery matrix')
            require(row['observer_failures_restored'] == 2 and row['cancelled_restored'] is True and
                    row['budget_restored'] is True and row['unsupported_format_restored'] is True and
                    predicate(row['retry']) is True, 'Recovery evidence')
            continue
        owned(row)
        if args.with_tests:
            require(all(len(p.get('test_variants', [])) == (5 if row['mode'] in TEST_FIXTURES else 0) for p in row['passes']), 'Required counterfactual matrix')
            if row['mode'] in TEST_FIXTURES:
                require(row['history']['primitive_presence_observation']['test_variants_requested'] is True, 'Required history counterfactuals')
        count += 1
        for mutate in (
            lambda r: r['passes'][0]['observation'].update(original_fragment_eligibility=True),
            lambda r: r['passes'][0]['observation']['completion_target'].update(rgb_reduction='replace'),
            lambda r: r['passes'][0]['after'][0].update(rgb_predicate=not r['passes'][0]['after'][0]['rgb_predicate']),
            lambda r: r['passes'][0].update(identity=99),
            lambda r: r['restored_storage'][0].__setitem__(0, r['restored_storage'][0][0] ^ 1),
            lambda r: r['passes'][0]['observation']['completion_shader'].update(requires_private_unblended_target=True)):
            changed = copy.deepcopy(row)
            mutate(changed)
            try:
                owned(changed)
            except (ValueError, KeyError, AssertionError):
                controls += 1
            else:
                raise ValueError('Corrupted accumulated predicate accepted')
        if args.with_tests and row['mode'] in TEST_FIXTURES:
            mutations = (
                lambda r: r['passes'][0]['test_variants'][0]['observation']['pipeline_test_state']['depth_stencil_after'].update(depth_enabled=True),
                lambda r: r['passes'][0]['test_variants'][0]['observation']['pipeline_test_state'].update(failure_reason='depth_failed'),
                lambda r: r['passes'][0]['test_variants'][0]['observation']['pipeline_test_state'].update(sample_mask_after=0),
                lambda r: r['history']['primitive_presence_observation']['observations'][0]['test_variants'].pop(),
                lambda r: r['history']['primitive_presence_observation']['observations'][0]['test_variants'][0].update(differs_from_preserved_tests=None),
                lambda r: r['history']['primitive_presence_observation']['observations'][0]['test_variants'][0]['passes'][1]['observation'].update(frame_sha256='changed'),
                lambda r: r['history']['primitive_presence_observation']['observations'][0]['test_variants'][0].update(failure_reason='depth_failed'),
                lambda r: r['history']['primitive_presence_observation']['observations'][0]['test_variants'][0]['passes'][1]['after'].update(rgb_predicate=None))
            for mutate in mutations:
                changed = copy.deepcopy(row)
                mutate(changed)
                try:
                    owned(changed)
                except (ValueError, KeyError, AssertionError, IndexError):
                    controls += 1
                else:
                    raise ValueError('Corrupted counterfactual evidence accepted')
        if row.get('history') is not None:
            for mutate in (
                lambda h: h['records'][0].update(system_id_presence_observation=999),
                lambda h: h['primitive_presence_observation']['observations'][0].update(system_primitive_id=999),
                lambda h: h['primitive_presence_observation']['observations'][0]['matching_primitive_ordinals'].append(999),
                lambda h: h['primitive_presence_observation']['observations'][0].update(color_completion_observed=None),
                lambda h: h['primitive_presence_observation']['observations'][0]['passes'][1]['observation'].update(frame_sha256='changed'),
                lambda h: h['primitive_presence_observation'].update(original_fragment_eligibility=True)):
                changed = copy.deepcopy(row)
                mutate(changed['history'])
                try:
                    owned(changed)
                except (ValueError, KeyError, AssertionError, IndexError):
                    controls += 1
                else:
                    raise ValueError('Corrupted presence/history association accepted')
    if args.real:
        directories = sorted(p for p in args.real.iterdir() if p.is_dir())
        require({p.name for p in directories} == {f'{case}-{vendor}' for case in ('gf2', 'bf1', 'bf1-first', 'bf1-earlier', 'helldivers')
                                                 for vendor in (0, 4318, 32902)}, 'Complete real matrix')
        for directory in directories:
            row = json.loads((directory / 'report.json').read_text(encoding='utf-8'))
            common(row, int(directory.name.rsplit('-', 1)[1]))
            code = copy.deepcopy(row['completion_shader'])
            code.update(kind='native_pixel_primitive_mask_shader', backend='native-pixel-primitive-mask/1')
            shader(code, (directory / 'original.dxbc').read_bytes(), (directory / 'marked.dxbc').read_bytes())
            require(row['probe']['status'] == 'ok' and row['probe']['original_storage_restored'] is True,
                    'Independent ordinary draw comparison')
            require((directory / 'before.bin').read_bytes() == (directory / 'restored.bin').read_bytes() and
                    (directory / 'original-draw.bin').read_bytes() == (directory / 'reference.bin').read_bytes(),
                    'Original byte restoration and ordinary replay')
            require(row['passing_samples'] == row['original_draw']['passing_samples'] ==
                    row['probe']['reference_passing_samples'], 'Unchanged whole-output counts')
            case = directory.name.rsplit('-', 1)[0]
            base = {'bf1-first': 'bf1', 'bf1-earlier': 'bf1', 'helldivers': 'hd'}.get(case, case)
            require(row['frame_sha256'] == FRAME_HASHES[base], 'Real frame identity')
            expected_id = 2044 if case == 'bf1' else 162 if case == 'bf1-earlier' else 0
            require(row['completion_shader']['primitive_value'] == expected_id, 'Real selected system ID')
            fmt = row['completion_target']['format']
            def rgb(raw):
                require(len(raw) == 4, 'Real typed pixel size')
                if fmt == 24:
                    value, = struct.unpack('<I', raw)
                    return [(value >> shift) & 1023 for shift in (0, 10, 20)]
                require(fmt in (28, 29), 'Unvalidated real predicate format')
                return list(raw[:3])
            cleared = rgb((directory / 'cleared.bin').read_bytes())
            marked = rgb((directory / 'marked.bin').read_bytes())
            require(cleared == [0, 0, 0], 'Real zero initial RGB')
            matched = all(v > 0 for v in marked)
            require(matched or marked == [0, 0, 0], 'Uninterpretable real RGB predicate')
            if case != 'bf1-first':
                require(matched, 'Previously observed last system ID must remain present')
            # The earlier BF1 candidate is measured; no expected eligibility is invented.
            real.append(dict(case=directory.name, primitive=row['completion_shader']['primitive_value'],
                             format=fmt, predicate_matched=matched, marked=list((directory / 'marked.bin').read_bytes()),
                             elapsed_ms=row['probe']['elapsed_ms']))
    jobs = []
    if args.history_real:
        require(args.real is not None, 'History jobs require independent real probes')
        from validate_native_draw_history_jobs import read, validate as validate_job
        directories = sorted(p for p in args.history_real.iterdir() if p.is_dir())
        require({p.name for p in directories} == {f'{case}-{vendor}' for case in ('gf2', 'bf1', 'helldivers')
                                                 for vendor in (0, 4318, 32902)}, 'Complete history job matrix')
        for directory in directories:
            job, result = read(directory)
            vendor = int(directory.name.rsplit('-', 1)[1])
            data = validate_job(job, result, vendor)
            require(job['options']['primitive_presence'] is True, 'Requested complete candidate observations')
            probe = args.real / directory.name
            baseline = json.loads((probe / 'report.json').read_text(encoding='utf-8'))
            require(data['frame_sha256'] == baseline['frame_sha256'] and data['event'] == baseline['event'] and
                    data['adapter'] == baseline['adapter'], 'History/probe execution context')
            for field, filename in (('before', 'before.bin'), ('after', 'reference.bin'), ('restored', 'restored.bin')):
                require(bytes(data[field]['bytes']) == (probe / filename).read_bytes(), 'Independent selected storage')
            for key in ('resource', 'x', 'y', 'sample'):
                require(data['selection'][key] == baseline['selection'][key], 'Selected probe pixel')
            observations = data['primitive_presence_observation']['observations']
            witnessed = {row['system_primitive_id']: row['color_completion_observed'] for row in observations}
            require(witnessed[baseline['completion_shader']['primitive_value']] is True, 'Last witnessed ID remains present')
            if directory.name.startswith('bf1-'):
                require(witnessed[0] is False, 'Earlier candidate agrees with its independent presence probe')
                require(witnessed[162] is True, 'Earlier positive agrees with its independent presence probe')
            jobs.append(dict(case=directory.name, context_id=result['context_id'], witnessed=witnessed,
                             elapsed_ms=result['execution']['elapsed_ms'], peak_working_set_bytes=result['execution']['peak_working_set_bytes']))
    with args.out.open('x', encoding='utf-8') as output:
        json.dump(dict(schema=1, status='ok', owned_cases=count, recovery_cases=3, negative_controls=controls,
                       real_cases=real, history_jobs=jobs, complete_fragment_history=False), output, indent=2)
        output.write('\n')
    print(json.dumps(dict(owned_cases=count, recovery_cases=3, negative_controls=controls, real_cases=len(real), history_jobs=len(jobs))))


if __name__ == '__main__':
    main()
